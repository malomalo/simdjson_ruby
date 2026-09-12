
#include <cmath>
#include <string>
#include <string_view>

// simdjson.h must precede ruby.h — Ruby's subst.h redefines snprintf,
// which breaks std::snprintf used inside simdjson.
#include "simdjson.h"
#include "ruby.h"
#include "ruby/encoding.h"

VALUE rb_mSimdjson;

VALUE rb_eSimdjsonParseError;

VALUE rb_cSimdjsonBuilder;
VALUE rb_eSimdjsonBuilderError;

using namespace simdjson;

// Convert tape to Ruby's Object
static VALUE make_ruby_object(dom::element element) {
    auto t = element.type();
    if (t == dom::element_type::ARRAY) {
        VALUE ary = rb_ary_new();
        for (dom::element x : element) {
            VALUE e = make_ruby_object(x);
            rb_ary_push(ary, e);
        }
        return ary;
    } else if (t == dom::element_type::OBJECT) {
        VALUE hash = rb_hash_new();
        for (dom::key_value_pair field : dom::object(element)) {
            std::string_view view(field.key);
            VALUE k = rb_utf8_str_new(view.data(), view.size());
            VALUE v = make_ruby_object(field.value);
            rb_hash_aset(hash, k, v);
        }
        return hash;
    } else if (t == dom::element_type::INT64) {
        return LONG2NUM(element.get<int64_t>());
    } else if (t == dom::element_type::UINT64) {
        return ULONG2NUM(element.get<uint64_t>());
    } else if (t == dom::element_type::DOUBLE) {
        return DBL2NUM(double(element));
    } else if (t == dom::element_type::STRING) {
        std::string_view view(element);
        return rb_utf8_str_new(view.data(), view.size());
    } else if (t == dom::element_type::BOOL) {
        return bool(element) ? Qtrue : Qfalse;
    } else if (t == dom::element_type::NULL_VALUE) {
        return Qnil;
    }
    // unknown case (bug)
    rb_raise(rb_eException, "[BUG] must not happen");
}

static VALUE rb_simdjson_parse(VALUE self, VALUE arg) {
    Check_Type(arg, T_STRING);

    dom::parser parser;
    padded_string str(RSTRING_PTR(arg), RSTRING_LEN(arg));
    dom::element doc;
    auto error = parser.parse(str).get(doc);
    if (error == SUCCESS) {
        return make_ruby_object(doc);
    }
    // TODO better error handling
    rb_raise(rb_eSimdjsonParseError, "parse error");
    return Qnil;
}

// ---------------------------------------------------------------------------
// Simdjson::Builder — a thin wrapper over simdjson::builder::string_builder.
//
// The string_builder is a low-level, structure-agnostic JSON serializer: it
// simply appends tokens to a growable buffer. The caller is responsible for
// emitting a well-formed document (commas, colons, matching braces). This
// mirrors the C++ API rather than hiding it, so callers keep full control and
// the SIMD-accelerated string escaping.
// ---------------------------------------------------------------------------

using simd_builder = builder::string_builder;

static void builder_free(void *ptr) { delete static_cast<simd_builder *>(ptr); }

static size_t builder_memsize(const void *ptr) {
    auto b = static_cast<const simd_builder *>(ptr);
    return sizeof(simd_builder) + (b ? b->size() : 0);
}

static const rb_data_type_t builder_data_type = {
    "Simdjson::Builder",
    {NULL, builder_free, builder_memsize},
    NULL,
    NULL,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE builder_allocate(VALUE klass) {
    // The underlying object is created in #initialize (it needs the capacity
    // argument). Wrapping NULL is safe: builder_free(NULL) is a no-op.
    return TypedData_Wrap_Struct(klass, &builder_data_type, NULL);
}

static simd_builder *get_builder(VALUE self) {
    simd_builder *b;
    TypedData_Get_Struct(self, simd_builder, &builder_data_type, b);
    if (b == NULL) {
        rb_raise(rb_eSimdjsonBuilderError, "uninitialized Simdjson::Builder");
    }
    return b;
}

static VALUE builder_initialize(int argc, VALUE *argv, VALUE self) {
    VALUE capacity;
    rb_scan_args(argc, argv, "01", &capacity);
    size_t initial = simd_builder::DEFAULT_INITIAL_CAPACITY;
    if (!NIL_P(capacity)) {
        if (!RB_INTEGER_TYPE_P(capacity)) {
            rb_raise(rb_eTypeError, "capacity must be an Integer");
        }
        // A negative capacity would wrap to a huge size_t under NUM2SIZET; reject
        // it up front rather than letting the allocation fail lazily at #view.
        if (RTEST(rb_funcall(capacity, rb_intern("negative?"), 0))) {
            rb_raise(rb_eArgError, "capacity must be non-negative");
        }
        initial = NUM2SIZET(capacity);
    }
    RTYPEDDATA_DATA(self) = new simd_builder(initial);
    return self;
}

// Return a UTF-8 encoded version of a String, so the bytes we append are valid
// UTF-8 and match the encoding view() labels the buffer with. UTF-8 strings are
// passed through untouched (their byte-content validity is the caller's concern,
// checkable via #validate_unicode); other encodings are transcoded, and bytes
// with no UTF-8 representation (e.g. binary strings) raise EncodingError rather
// than silently corrupting the output.
static VALUE to_utf8(VALUE str) {
    rb_encoding *utf8 = rb_utf8_encoding();
    if (rb_enc_get(str) == utf8) {
        return str;
    }
    return rb_str_encode(str, rb_enc_from_encoding(utf8), 0, Qnil);
}

// Append a Ruby value as a JSON value, dispatching on its type. Strings and
// symbols are escaped and quoted; the numeric/boolean/nil literals are emitted
// verbatim. Integers outside the int64 range are serialized via their decimal
// string so arbitrary-precision values remain valid JSON numbers.
static void append_value(simd_builder *b, VALUE v) {
    switch (TYPE(v)) {
        case T_NIL:
            b->append_null();
            break;
        case T_TRUE:
            b->append(true);
            break;
        case T_FALSE:
            b->append(false);
            break;
        case T_FIXNUM:
            b->append(static_cast<int64_t>(NUM2LL(v)));
            break;
        case T_BIGNUM: {
            VALUE s = rb_big2str(v, 10);
            b->append_raw(std::string_view(RSTRING_PTR(s), RSTRING_LEN(s)));
            break;
        }
        case T_FLOAT: {
            double d = RFLOAT_VALUE(v);
            // JSON has no representation for Infinity or NaN, and simdjson's
            // to_chars is only defined for finite values (it would emit garbage
            // for inf/nan). Reject them rather than produce unparseable output.
            if (!std::isfinite(d)) {
                rb_raise(rb_eSimdjsonBuilderError, "cannot serialize non-finite Float %" PRIsVALUE " to JSON", v);
            }
            b->append(d);
            break;
        }
        case T_STRING: {
            VALUE s = to_utf8(v);
            b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(s), RSTRING_LEN(s)));
            break;
        }
        case T_SYMBOL: {
            VALUE s = to_utf8(rb_sym2str(v));
            b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(s), RSTRING_LEN(s)));
            break;
        }
        default:
            rb_raise(rb_eTypeError, "cannot append %" PRIsVALUE " to Simdjson::Builder", rb_obj_class(v));
    }
}

// Escape and quote a string-like value for use as an object key.
static void append_string_token(simd_builder *b, VALUE v) {
    if (RB_TYPE_P(v, T_SYMBOL)) {
        v = rb_sym2str(v);
    }
    Check_Type(v, T_STRING);
    v = to_utf8(v);
    b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(v), RSTRING_LEN(v)));
}

static VALUE builder_start_object(VALUE self) {
    get_builder(self)->start_object();
    return self;
}

static VALUE builder_end_object(VALUE self) {
    get_builder(self)->end_object();
    return self;
}

static VALUE builder_start_array(VALUE self) {
    get_builder(self)->start_array();
    return self;
}

static VALUE builder_end_array(VALUE self) {
    get_builder(self)->end_array();
    return self;
}

static VALUE builder_append_comma(VALUE self) {
    get_builder(self)->append_comma();
    return self;
}

static VALUE builder_append_colon(VALUE self) {
    get_builder(self)->append_colon();
    return self;
}

static VALUE builder_append(VALUE self, VALUE v) {
    append_value(get_builder(self), v);
    return self;
}

static VALUE builder_append_key(VALUE self, VALUE key) {
    append_string_token(get_builder(self), key);
    return self;
}

// Convenience: emit `"key":value`. The key is escaped and quoted; the value is
// dispatched exactly like #append.
static VALUE builder_append_key_value(VALUE self, VALUE key, VALUE value) {
    simd_builder *b = get_builder(self);
    append_string_token(b, key);
    b->append_colon();
    append_value(b, value);
    return self;
}

// Append bytes verbatim, without escaping or quoting. The caller is
// responsible for producing valid JSON.
static VALUE builder_append_raw(VALUE self, VALUE str) {
    Check_Type(str, T_STRING);
    get_builder(self)->append_raw(std::string_view(RSTRING_PTR(str), RSTRING_LEN(str)));
    return self;
}

static VALUE builder_view(VALUE self) {
    simd_builder *b = get_builder(self);
    std::string_view result;
    auto error = b->view().get(result);
    if (error) {
        rb_raise(rb_eSimdjsonBuilderError, "builder error: %s", error_message(error));
    }
    return rb_utf8_str_new(result.data(), result.size());
}

static VALUE builder_size(VALUE self) { return SIZET2NUM(get_builder(self)->size()); }

static VALUE builder_clear(VALUE self) {
    get_builder(self)->clear();
    return self;
}

static VALUE builder_validate_unicode(VALUE self) {
    return get_builder(self)->validate_unicode() ? Qtrue : Qfalse;
}

extern "C" {

void Init_simdjson(void) {
    rb_mSimdjson = rb_define_module("Simdjson");
    rb_eSimdjsonParseError = rb_define_class_under(rb_mSimdjson, "ParseError", rb_eStandardError);
    rb_define_module_function(rb_mSimdjson, "parse", reinterpret_cast<VALUE (*)(...)>(rb_simdjson_parse), 1);

    rb_eSimdjsonBuilderError = rb_define_class_under(rb_mSimdjson, "BuilderError", rb_eStandardError);

    rb_cSimdjsonBuilder = rb_define_class_under(rb_mSimdjson, "Builder", rb_cObject);
    rb_define_alloc_func(rb_cSimdjsonBuilder, builder_allocate);
    rb_define_method(rb_cSimdjsonBuilder, "initialize", reinterpret_cast<VALUE (*)(...)>(builder_initialize), -1);
    rb_define_method(rb_cSimdjsonBuilder, "start_object", reinterpret_cast<VALUE (*)(...)>(builder_start_object), 0);
    rb_define_method(rb_cSimdjsonBuilder, "end_object", reinterpret_cast<VALUE (*)(...)>(builder_end_object), 0);
    rb_define_method(rb_cSimdjsonBuilder, "start_array", reinterpret_cast<VALUE (*)(...)>(builder_start_array), 0);
    rb_define_method(rb_cSimdjsonBuilder, "end_array", reinterpret_cast<VALUE (*)(...)>(builder_end_array), 0);
    rb_define_method(rb_cSimdjsonBuilder, "append_comma", reinterpret_cast<VALUE (*)(...)>(builder_append_comma), 0);
    rb_define_method(rb_cSimdjsonBuilder, "append_colon", reinterpret_cast<VALUE (*)(...)>(builder_append_colon), 0);
    rb_define_method(rb_cSimdjsonBuilder, "append", reinterpret_cast<VALUE (*)(...)>(builder_append), 1);
    rb_define_method(rb_cSimdjsonBuilder, "append_key", reinterpret_cast<VALUE (*)(...)>(builder_append_key), 1);
    rb_define_method(rb_cSimdjsonBuilder, "append_key_value",
                     reinterpret_cast<VALUE (*)(...)>(builder_append_key_value), 2);
    rb_define_method(rb_cSimdjsonBuilder, "append_raw", reinterpret_cast<VALUE (*)(...)>(builder_append_raw), 1);
    rb_define_method(rb_cSimdjsonBuilder, "view", reinterpret_cast<VALUE (*)(...)>(builder_view), 0);
    rb_define_method(rb_cSimdjsonBuilder, "to_s", reinterpret_cast<VALUE (*)(...)>(builder_view), 0);
    rb_define_method(rb_cSimdjsonBuilder, "size", reinterpret_cast<VALUE (*)(...)>(builder_size), 0);
    rb_define_method(rb_cSimdjsonBuilder, "length", reinterpret_cast<VALUE (*)(...)>(builder_size), 0);
    rb_define_method(rb_cSimdjsonBuilder, "clear", reinterpret_cast<VALUE (*)(...)>(builder_clear), 0);
    rb_define_method(rb_cSimdjsonBuilder, "validate_unicode",
                     reinterpret_cast<VALUE (*)(...)>(builder_validate_unicode), 0);
}
}
