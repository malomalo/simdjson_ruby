
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

// simdjson.h must precede ruby.h — Ruby's subst.h redefines snprintf,
// which breaks std::snprintf used inside simdjson.
#include "simdjson.h"
#include "ruby.h"

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

    // rb_raise unwinds with longjmp, which does not run C++ destructors, so the
    // parser and padded_string (both owning heap buffers) must go out of scope
    // before we raise. Materialize the whole Ruby object graph inside this scope
    // — doc references data owned by parser/str — then raise afterwards.
    VALUE result = Qnil;
    error_code error;
    {
        dom::parser parser;
        padded_string str(RSTRING_PTR(arg), RSTRING_LEN(arg));
        dom::element doc;
        error = parser.parse(str).get(doc);
        if (error == SUCCESS) {
            result = make_ruby_object(doc);
        }
    }
    if (error != SUCCESS) {
        // TODO better error handling
        rb_raise(rb_eSimdjsonParseError, "parse error");
    }
    return result;
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

static ID id_write;

// One open container on the writer's nesting stack. `has_child` tracks whether a
// member/element has been emitted yet (so we know when a comma is due), and
// `need_value` (objects only) marks that a key + colon were written by #push_key
// and the next value fills that slot rather than starting a new member.
enum class frame_kind : char { OBJECT, ARRAY };
struct frame {
    frame_kind kind;
    bool has_child;
    bool need_value;
};

// A builder plus an optional streaming sink and the writer's nesting stack. When
// `io` is set, the builder is a streaming writer: once its buffer grows past
// `buffer_size` bytes the content is written to `io` (via #write) and the buffer
// is reset, so a large document is delivered in bounded chunks instead of being
// held whole. With no `io` it is the plain buffer-backed builder, read back with
// #buffer. The byte buffer's `clear()` does not touch `stack`, so flushing
// mid-document is safe: the structural state lives here, not in the bytes.
struct builder_state {
    simd_builder *b;
    VALUE io;            // the sink, or Qnil for buffer-only
    size_t buffer_size;  // flush threshold when streaming
    std::vector<frame> stack;
};

static void builder_free(void *ptr) {
    auto s = static_cast<builder_state *>(ptr);
    if (s) {
        delete s->b;
        delete s;
    }
}

static size_t builder_memsize(const void *ptr) {
    auto s = static_cast<const builder_state *>(ptr);
    if (s == NULL) {
        return sizeof(builder_state);
    }
    return sizeof(builder_state) + (s->b ? sizeof(simd_builder) + s->b->size() : 0) +
           s->stack.capacity() * sizeof(frame);
}

static void builder_mark(void *ptr) {
    auto s = static_cast<builder_state *>(ptr);
    if (s && !NIL_P(s->io)) {
        rb_gc_mark(s->io);
    }
}

static const rb_data_type_t builder_data_type = {
    "Simdjson::Builder",
    {builder_mark, builder_free, builder_memsize},
    NULL,
    NULL,
    RUBY_TYPED_FREE_IMMEDIATELY,
};

static VALUE builder_allocate(VALUE klass) {
    // The state is created in #initialize (it needs the arguments). Wrapping
    // NULL is safe: builder_free(NULL) is a no-op.
    return TypedData_Wrap_Struct(klass, &builder_data_type, NULL);
}

static builder_state *get_state(VALUE self) {
    builder_state *s;
    TypedData_Get_Struct(self, builder_state, &builder_data_type, s);
    if (s == NULL) {
        rb_raise(rb_eSimdjsonBuilderError, "uninitialized Simdjson::Builder");
    }
    return s;
}

// Write the buffered bytes to the sink and reset the buffer. A no-op when not
// streaming or when there is nothing buffered. The bytes are copied into a Ruby
// String before the buffer is cleared, so an #write that raises loses only the
// in-flight chunk (the render is unwinding anyway).
static void builder_do_flush(builder_state *s) {
    if (NIL_P(s->io)) {
        return;
    }
    std::string_view view;
    auto error = s->b->view().get(view);
    if (error) {
        rb_raise(rb_eSimdjsonBuilderError, "builder error: %s", error_message(error));
    }
    if (view.size() == 0) {
        return;
    }
    VALUE chunk = rb_utf8_str_new(view.data(), view.size());
    s->b->clear();
    rb_funcall(s->io, id_write, 1, chunk);
}

// Flush once the buffer has grown past the streaming threshold. A plain C
// branch on the hot path -- the Ruby #write happens only per buffer_size, not
// per token.
static inline void builder_maybe_flush(builder_state *s) {
    if (!NIL_P(s->io) && s->b->size() >= s->buffer_size) {
        builder_do_flush(s);
    }
}

static const size_t DEFAULT_BUFFER_SIZE = 4096;

// Simdjson::Builder.new(capacity = nil, io: nil, buffer_size: 4096)
static VALUE builder_initialize(int argc, VALUE *argv, VALUE self) {
    VALUE capacity, opts;
    rb_scan_args(argc, argv, "01:", &capacity, &opts);

    size_t initial = simd_builder::DEFAULT_INITIAL_CAPACITY;
    if (!NIL_P(capacity)) {
        if (!RB_INTEGER_TYPE_P(capacity)) {
            rb_raise(rb_eTypeError, "capacity must be an Integer");
        }
        // A negative capacity would wrap to a huge size_t under NUM2SIZET; reject
        // it up front rather than letting the allocation fail lazily at #buffer.
        if (RTEST(rb_funcall(capacity, rb_intern("negative?"), 0))) {
            rb_raise(rb_eArgError, "capacity must be non-negative");
        }
        initial = NUM2SIZET(capacity);
    }

    VALUE io = Qnil;
    size_t buffer_size = DEFAULT_BUFFER_SIZE;
    if (!NIL_P(opts)) {
        VALUE v_io = rb_hash_aref(opts, ID2SYM(rb_intern("io")));
        if (!NIL_P(v_io)) {
            if (!rb_respond_to(v_io, id_write)) {
                rb_raise(rb_eTypeError, "io must respond to #write");
            }
            io = v_io;
        }
        VALUE v_bs = rb_hash_aref(opts, ID2SYM(rb_intern("buffer_size")));
        if (!NIL_P(v_bs)) {
            if (!RB_INTEGER_TYPE_P(v_bs) ||
                RTEST(rb_funcall(v_bs, rb_intern("<="), 1, INT2FIX(0)))) {
                rb_raise(rb_eArgError, "buffer_size must be a positive Integer");
            }
            buffer_size = NUM2SIZET(v_bs);
        }
    }

    // Guard against a second #initialize leaking the state from the first. On
    // the normal path the wrapped pointer is NULL (see builder_allocate).
    builder_free(RTYPEDDATA_DATA(self));
    auto s = new builder_state{new simd_builder(initial), io, buffer_size};
    RTYPEDDATA_DATA(self) = s;
    return self;
}

// Cap generated nesting at the same depth the parser accepts by default
// (simdjson::DEFAULT_MAX_DEPTH), which also stops a self-referential structure
// (e.g. a = []; a << a) from recursing until the C stack overflows.
static const int MAX_NESTING_DEPTH = static_cast<int>(simdjson::DEFAULT_MAX_DEPTH);

// Forward declaration: Arrays and Hashes append their elements recursively.
static void append_value(simd_builder *b, VALUE v, int depth);

// Convert a Hash key to its JSON string form. Symbols and Strings are used
// directly; any other key is stringified with #to_s, matching JSON.generate.
static void append_hash_key(simd_builder *b, VALUE key) {
    if (RB_TYPE_P(key, T_SYMBOL)) {
        key = rb_sym2str(key);
    } else if (!RB_TYPE_P(key, T_STRING)) {
        key = rb_obj_as_string(key);
    }
    b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(key), RSTRING_LEN(key)));
}

struct hash_append_ctx {
    simd_builder *b;
    int depth;
    bool first;
};

// rb_hash_foreach callback: emit `"key":value`, comma-separated. rb_hash_foreach
// wraps iteration in rb_ensure, so a raise from append_value here is safe.
static int hash_append_i(VALUE key, VALUE val, VALUE arg) {
    hash_append_ctx *ctx = reinterpret_cast<hash_append_ctx *>(arg);
    if (!ctx->first) {
        ctx->b->append_comma();
    }
    ctx->first = false;
    append_hash_key(ctx->b, key);
    ctx->b->append_colon();
    append_value(ctx->b, val, ctx->depth + 1);
    return ST_CONTINUE;
}

// Append a Ruby value as a JSON value, dispatching on its type. Strings and
// symbols are escaped and quoted; the numeric/boolean/nil literals are emitted
// verbatim. Integers outside the int64 range are serialized via their decimal
// string so arbitrary-precision values remain valid JSON numbers. Arrays and
// Hashes are emitted as JSON arrays/objects, recursing into their elements
// (Hash keys are stringified, values dispatched like any other value).
//
// String bytes are appended as-is (escaped but not transcoded), mirroring the
// parser, which likewise does not inspect Ruby encodings and relies on
// simdjson's own UTF-8 handling. Callers wanting a UTF-8 guarantee on the
// output can check it with #validate_unicode.
static void append_value(simd_builder *b, VALUE v, int depth) {
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
        case T_STRING:
            b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(v), RSTRING_LEN(v)));
            break;
        case T_SYMBOL: {
            VALUE s = rb_sym2str(v);
            b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(s), RSTRING_LEN(s)));
            break;
        }
        case T_ARRAY: {
            if (depth >= MAX_NESTING_DEPTH) {
                rb_raise(rb_eSimdjsonBuilderError, "nesting is too deep");
            }
            b->start_array();
            long n = RARRAY_LEN(v);
            for (long i = 0; i < n; i++) {
                if (i > 0) {
                    b->append_comma();
                }
                append_value(b, RARRAY_AREF(v, i), depth + 1);
            }
            b->end_array();
            break;
        }
        case T_HASH: {
            if (depth >= MAX_NESTING_DEPTH) {
                rb_raise(rb_eSimdjsonBuilderError, "nesting is too deep");
            }
            b->start_object();
            hash_append_ctx ctx = {b, depth, true};
            rb_hash_foreach(v, hash_append_i, reinterpret_cast<VALUE>(&ctx));
            b->end_object();
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
    } else {
        StringValue(v);  // coerce via #to_str, or raise TypeError
    }
    b->escape_and_append_with_quotes(std::string_view(RSTRING_PTR(v), RSTRING_LEN(v)));
}

static VALUE builder_append(VALUE self, VALUE v) {
    builder_state *s = get_state(self);
    append_value(s->b, v, 0);
    builder_maybe_flush(s);
    return self;
}

// Emit the separator that must precede the next item in the current container,
// and record that the container now has a child. In an object that means the
// comma between members plus the member's `"key":` (a key is required unless one
// was already written by #push_key, tracked via need_value); in an array, just
// the comma between elements (a key is not allowed). At the top level (empty
// stack) there is no separator — the item is a bare document-level value.
static void writer_before_item(builder_state *s, VALUE key) {
    if (s->stack.empty()) {
        return;
    }
    frame &top = s->stack.back();
    if (top.kind == frame_kind::OBJECT) {
        if (top.need_value) {
            top.need_value = false;  // fills the slot opened by #push_key
            return;
        }
        if (NIL_P(key)) {
            rb_raise(rb_eSimdjsonBuilderError, "a key is required for an object member");
        }
        if (top.has_child) {
            s->b->append_comma();
        }
        append_string_token(s->b, key);
        s->b->append_colon();
        top.has_child = true;
    } else {
        if (!NIL_P(key)) {
            rb_raise(rb_eSimdjsonBuilderError, "a key is not allowed in an array");
        }
        if (top.has_child) {
            s->b->append_comma();
        }
        top.has_child = true;
    }
}

// #push_value(value, key = nil) — append a value (any type #append accepts,
// including nested Arrays/Hashes). Inside an object a key is required (unless one
// was set with #push_key); inside an array a key is forbidden.
static VALUE builder_push_value(int argc, VALUE *argv, VALUE self) {
    VALUE value, key;
    rb_scan_args(argc, argv, "11", &value, &key);
    builder_state *s = get_state(self);
    writer_before_item(s, key);
    append_value(s->b, value, 0);
    builder_maybe_flush(s);
    return self;
}

// #push_object(key = nil) — open an object as the next item of the current
// container. Balance it with #pop.
static VALUE builder_push_object(int argc, VALUE *argv, VALUE self) {
    VALUE key;
    rb_scan_args(argc, argv, "01", &key);
    builder_state *s = get_state(self);
    writer_before_item(s, key);
    s->b->start_object();
    s->stack.push_back(frame{frame_kind::OBJECT, false, false});
    builder_maybe_flush(s);
    return self;
}

// #push_array(key = nil) — open an array as the next item of the current
// container. Balance it with #pop.
static VALUE builder_push_array(int argc, VALUE *argv, VALUE self) {
    VALUE key;
    rb_scan_args(argc, argv, "01", &key);
    builder_state *s = get_state(self);
    writer_before_item(s, key);
    s->b->start_array();
    s->stack.push_back(frame{frame_kind::ARRAY, false, false});
    builder_maybe_flush(s);
    return self;
}

// #push_key(key) — emit an object member's key (and its colon) on its own; the
// next #push_value/#push_object/#push_array supplies the value.
static VALUE builder_push_key(VALUE self, VALUE key) {
    builder_state *s = get_state(self);
    if (s->stack.empty() || s->stack.back().kind != frame_kind::OBJECT) {
        rb_raise(rb_eSimdjsonBuilderError, "push_key is only valid inside an object");
    }
    frame &top = s->stack.back();
    if (top.need_value) {
        rb_raise(rb_eSimdjsonBuilderError, "push_key called twice without a value");
    }
    if (top.has_child) {
        s->b->append_comma();
    }
    append_string_token(s->b, key);
    s->b->append_colon();
    top.has_child = true;
    top.need_value = true;
    builder_maybe_flush(s);
    return self;
}

// #pop — close the current object or array.
static VALUE builder_pop(VALUE self) {
    builder_state *s = get_state(self);
    if (s->stack.empty()) {
        rb_raise(rb_eSimdjsonBuilderError, "pop with no open object or array");
    }
    frame top = s->stack.back();
    if (top.kind == frame_kind::OBJECT) {
        if (top.need_value) {
            rb_raise(rb_eSimdjsonBuilderError, "pop after push_key without a value");
        }
        s->b->end_object();
    } else {
        s->b->end_array();
    }
    s->stack.pop_back();
    builder_maybe_flush(s);
    return self;
}

// #pop_all — close every open container, finishing the document.
static VALUE builder_pop_all(VALUE self) {
    builder_state *s = get_state(self);
    while (!s->stack.empty()) {
        frame top = s->stack.back();
        if (top.kind == frame_kind::OBJECT) {
            if (top.need_value) {
                rb_raise(rb_eSimdjsonBuilderError, "pop_all after push_key without a value");
            }
            s->b->end_object();
        } else {
            s->b->end_array();
        }
        s->stack.pop_back();
    }
    builder_maybe_flush(s);
    return self;
}

// Append bytes verbatim, without escaping or quoting. The caller is
// responsible for producing valid JSON.
static VALUE builder_append_raw(VALUE self, VALUE str) {
    StringValue(str);  // coerce via #to_str, or raise TypeError
    builder_state *s = get_state(self);
    s->b->append_raw(std::string_view(RSTRING_PTR(str), RSTRING_LEN(str)));
    builder_maybe_flush(s);
    return self;
}

// Force any buffered bytes out to the streaming io now (a no-op when not
// streaming). Used at capture boundaries and to finish a document.
static VALUE builder_flush(VALUE self) {
    builder_do_flush(get_state(self));
    return self;
}

// The bytes currently buffered but not yet flushed. When streaming this is only
// the tail since the last flush, not the whole document; a buffer-only builder
// holds the whole document here.
static VALUE builder_buffer(VALUE self) {
    std::string_view result;
    auto error = get_state(self)->b->view().get(result);
    if (error) {
        rb_raise(rb_eSimdjsonBuilderError, "builder error: %s", error_message(error));
    }
    return rb_utf8_str_new(result.data(), result.size());
}

static VALUE builder_size(VALUE self) { return SIZET2NUM(get_state(self)->b->size()); }

static VALUE builder_clear(VALUE self) {
    builder_state *s = get_state(self);
    s->b->clear();
    s->stack.clear();  // discard any open containers; start a fresh document
    return self;
}

static VALUE builder_validate_unicode(VALUE self) {
    return get_state(self)->b->validate_unicode() ? Qtrue : Qfalse;
}

extern "C" {

// Ruby 3.x's headers provide C++ overloads of rb_define_method et al. that
// accept correctly-typed function pointers directly, so no ANYARGS cast is
// needed (or wanted — the cast is technically undefined behavior).
void Init_simdjson(void) {
    id_write = rb_intern("write");
    rb_mSimdjson = rb_define_module("Simdjson");
    rb_eSimdjsonParseError = rb_define_class_under(rb_mSimdjson, "ParseError", rb_eStandardError);
    rb_define_module_function(rb_mSimdjson, "parse", rb_simdjson_parse, 1);

    rb_eSimdjsonBuilderError = rb_define_class_under(rb_mSimdjson, "BuilderError", rb_eStandardError);

    rb_cSimdjsonBuilder = rb_define_class_under(rb_mSimdjson, "Builder", rb_cObject);
    rb_define_alloc_func(rb_cSimdjsonBuilder, builder_allocate);
    rb_define_method(rb_cSimdjsonBuilder, "initialize", builder_initialize, -1);
    rb_define_method(rb_cSimdjsonBuilder, "append", builder_append, 1);
    rb_define_method(rb_cSimdjsonBuilder, "append_raw", builder_append_raw, 1);
    rb_define_method(rb_cSimdjsonBuilder, "push_object", builder_push_object, -1);
    rb_define_method(rb_cSimdjsonBuilder, "push_array", builder_push_array, -1);
    rb_define_method(rb_cSimdjsonBuilder, "push_value", builder_push_value, -1);
    rb_define_method(rb_cSimdjsonBuilder, "push_key", builder_push_key, 1);
    rb_define_method(rb_cSimdjsonBuilder, "pop", builder_pop, 0);
    rb_define_method(rb_cSimdjsonBuilder, "pop_all", builder_pop_all, 0);
    rb_define_method(rb_cSimdjsonBuilder, "flush", builder_flush, 0);
    rb_define_method(rb_cSimdjsonBuilder, "buffer", builder_buffer, 0);
    rb_define_method(rb_cSimdjsonBuilder, "to_s", builder_buffer, 0);
    rb_define_method(rb_cSimdjsonBuilder, "size", builder_size, 0);
    rb_define_method(rb_cSimdjsonBuilder, "length", builder_size, 0);
    rb_define_method(rb_cSimdjsonBuilder, "clear", builder_clear, 0);
    rb_define_method(rb_cSimdjsonBuilder, "validate_unicode", builder_validate_unicode, 0);
}
}
