# frozen_string_literal: true

require 'test_helper'
require 'stringio'

class BuilderTest < Minitest::Test
  def setup
    @b = Simdjson::Builder.new
  end

  def test_empty_object
    @b.start_object.end_object
    assert_equal '{}', @b.buffer
  end

  def test_empty_array
    @b.start_array.end_array
    assert_equal '[]', @b.buffer
  end

  SCALARS = { 42 => '42', -7 => '-7', true => 'true', false => 'false',
              nil => 'null', 1.5 => '1.5', 'hi' => '"hi"' }.freeze

  def test_scalars
    SCALARS.each { |value, expected| assert_equal expected, @b.clear.append(value).buffer }
  end

  def test_symbol_is_appended_as_string
    assert_equal '"foo"', @b.append(:foo).buffer
  end

  def test_bignum_serialized_as_number
    big = 123_456_789_012_345_678_901_234_567_890
    assert_equal big.to_s, @b.append(big).buffer
  end

  def test_string_escaping
    @b.append("a \"quote\" and \\ and\n newline")
    assert_equal %("a \\"quote\\" and \\\\ and\\n newline"), @b.buffer
  end

  def test_unicode_is_preserved
    @b.append('café')
    assert_equal '"café"', @b.buffer
    assert @b.validate_unicode
  end

  # Like the parser, the builder does not inspect Ruby encodings or transcode;
  # it appends bytes as-is and leaves UTF-8 verification to #validate_unicode.
  def test_append_does_not_transcode_and_validate_unicode_flags_it
    @b.append('café'.encode('ISO-8859-1'))
    refute @b.validate_unicode
  end

  def test_buffer_is_utf8
    assert_equal Encoding::UTF_8, @b.append('x').buffer.encoding
  end

  def test_append_raw_is_not_escaped
    @b.append_raw('[1,2,3]')
    assert_equal '[1,2,3]', @b.buffer
  end

  def test_append_key_and_colon
    @b.start_object.append_key('name').append_colon.append('Ada').end_object
    assert_equal '{"name":"Ada"}', @b.buffer
  end

  def test_append_key_value
    @b.start_object.append_key_value('year', 2017).end_object
    assert_equal '{"year":2017}', @b.buffer
  end

  def test_key_value_escapes_key
    @b.append_key_value('a"b', 1)
    assert_equal '"a\\"b":1', @b.buffer
  end

  def append_array(values)
    @b.start_array
    values.each_with_index do |v, i|
      @b.append_comma if i.positive?
      @b.append(v)
    end
    @b.end_array
  end

  def test_nested_document_round_trips
    @b.start_object
    @b.append_key_value('make', 'Toyota').append_comma
    @b.append_key_value('year', 2017).append_comma
    @b.append_key('tires').append_colon
    append_array([30.0, 30.2, 30.5])
    @b.end_object

    assert_equal({ 'make' => 'Toyota', 'year' => 2017, 'tires' => [30.0, 30.2, 30.5] },
                 Simdjson.parse(@b.buffer))
  end

  def test_size_tracks_written_bytes
    assert_equal 0, @b.size
    @b.append('ab') # => "ab" (4 bytes with quotes)
    assert_equal 4, @b.size
  end

  def test_clear_resets_and_allows_reuse
    @b.append(1).clear
    assert_equal 0, @b.size
    assert_equal '2', @b.append(2).buffer
  end

  def test_initial_capacity_argument
    b = Simdjson::Builder.new(8)
    b.append('a longer string than eight bytes')
    assert_equal '"a longer string than eight bytes"', b.buffer
  end

  def test_negative_capacity_raises
    assert_raises(ArgumentError) { Simdjson::Builder.new(-1) }
  end

  def test_non_integer_capacity_raises
    assert_raises(TypeError) { Simdjson::Builder.new('big') }
  end

  def test_reinitialize_resets_and_does_not_double_free
    @b.append('discarded')
    @b.send(:initialize, 8)
    assert_equal 0, @b.size
    assert_equal '"x"', @b.append('x').buffer
    GC.start
  end

  def test_to_s_is_buffer
    @b.append('x')
    assert_equal @b.buffer, @b.to_s
  end

  def test_append_array
    @b.append([1, 'two', 3.5, true, false, nil])
    assert_equal '[1,"two",3.5,true,false,null]', @b.buffer
    assert_equal [1, 'two', 3.5, true, false, nil], Simdjson.parse(@b.buffer)
  end

  def test_append_empty_array
    assert_equal '[]', @b.append([]).buffer
  end

  def test_append_hash
    @b.append({ 'name' => 'Ada', 'year' => 2017 })
    assert_equal '{"name":"Ada","year":2017}', @b.buffer
  end

  def test_append_empty_hash
    assert_equal '{}', @b.append({}).buffer
  end

  def test_append_hash_symbol_keys
    assert_equal '{"a":1,"b":2}', @b.append({ a: 1, b: 2 }).buffer
  end

  def test_append_hash_stringifies_non_string_keys
    assert_equal '{"1":2}', @b.append({ 1 => 2 }).buffer
  end

  def test_append_nested_structure_round_trips
    doc = { 'make' => 'Toyota', 'year' => 2017, 'tires' => [30.0, 30.2, 30.5],
            'meta' => { 'new' => true, 'tags' => %w[a b] } }
    @b.append(doc)
    assert_equal doc, Simdjson.parse(@b.buffer)
  end

  def test_append_key_value_with_container_value
    @b.start_object.append_key_value('list', [1, 2, 3]).end_object
    assert_equal({ 'list' => [1, 2, 3] }, Simdjson.parse(@b.buffer))
  end

  def test_append_rejects_excessive_nesting
    deep = []
    cursor = deep
    2000.times do
      inner = []
      cursor << inner
      cursor = inner
    end
    assert_raises(Simdjson::BuilderError) { @b.append(deep) }
  end

  def test_append_rejects_self_referential_structure
    a = []
    a << a
    assert_raises(Simdjson::BuilderError) { @b.append(a) }
  end

  def test_append_rejects_non_finite_floats
    [Float::INFINITY, -Float::INFINITY, Float::NAN, 1.0 / 0.0].each do |value|
      assert_raises(Simdjson::BuilderError) { @b.clear.append(value) }
    end
  end

  def test_append_rejects_unsupported_type
    assert_raises(TypeError) { @b.append(Object.new) }
  end

  def test_append_raw_requires_string
    assert_raises(TypeError) { @b.append_raw(123) }
  end

  def test_append_raw_accepts_to_str
    obj = Object.new
    def obj.to_str = '[1,2]'
    @b.append_raw(obj)
    assert_equal '[1,2]', @b.buffer
  end

  def test_append_key_accepts_to_str
    key = Object.new
    def key.to_str = 'k'
    @b.start_object.append_key_value(key, 1).end_object
    assert_equal '{"k":1}', @b.buffer
  end

  def test_chaining_returns_self
    assert_same @b, @b.start_array
    assert_same @b, @b.append(1)
    assert_same @b, @b.end_array
  end

  # --- Streaming (io: / buffer_size: / flush) ---

  def test_streams_to_io_as_the_buffer_fills
    io = StringIO.new
    b = Simdjson::Builder.new(io: io, buffer_size: 16)
    b.start_array
    10.times { |i| b.append_comma if i > 0; b.append("element-#{i}") }
    # Past the 16-byte buffer, content has already been handed to the io before
    # we ask for a flush -- i.e. it streamed rather than buffering the whole array.
    refute_empty io.string, 'expected bytes to reach the io before #flush'
    b.end_array
    b.flush
    assert_equal((0...10).map { |i| "element-#{i}" }, Simdjson.parse(io.string))
  end

  def test_flush_writes_the_remaining_bytes
    io = StringIO.new
    b = Simdjson::Builder.new(io: io, buffer_size: 1 << 20) # large: nothing auto-flushes
    b.start_object.append_key_value('a', 1).end_object
    assert_empty io.string, 'nothing should be written until the buffer fills or #flush'
    b.flush
    assert_equal '{"a":1}', io.string
  end

  def test_buffer_holds_only_the_unflushed_tail_when_streaming
    io = StringIO.new
    b = Simdjson::Builder.new(io: io, buffer_size: 8)
    b.append_raw('xxxxxxxxxx') # 10 bytes > 8 -> flushed to io, buffer reset
    assert_equal 'xxxxxxxxxx', io.string
    assert_equal '', b.buffer
  end

  def test_flush_is_a_noop_without_an_io
    @b.append(1)
    assert_same @b, @b.flush
    assert_equal '1', @b.buffer
  end

  def test_accepts_capacity_and_io_together
    io = StringIO.new
    b = Simdjson::Builder.new(1024, io: io, buffer_size: 4096)
    b.append(1)
    b.flush
    assert_equal '1', io.string
  end

  def test_io_must_respond_to_write
    assert_raises(TypeError) { Simdjson::Builder.new(io: Object.new) }
  end

  def test_buffer_size_must_be_a_positive_integer
    assert_raises(ArgumentError) { Simdjson::Builder.new(io: StringIO.new, buffer_size: 0) }
    assert_raises(ArgumentError) { Simdjson::Builder.new(io: StringIO.new, buffer_size: -1) }
  end
end
