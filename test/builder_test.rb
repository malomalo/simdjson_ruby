# frozen_string_literal: true

require 'test_helper'
require 'stringio'

class BuilderTest < Minitest::Test
  def setup
    @b = Simdjson::Builder.new
  end

  # --- #append: whole values and bulk structures ---

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

  # --- #append_raw: pre-serialized bytes, spliced verbatim ---

  def test_append_raw_is_not_escaped
    @b.append_raw('[1,2,3]')
    assert_equal '[1,2,3]', @b.buffer
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

  # --- Writer: push/pop with automatic commas and colons ---

  def test_push_empty_object
    @b.push_object.pop
    assert_equal '{}', @b.buffer
  end

  def test_push_empty_array
    @b.push_array.pop
    assert_equal '[]', @b.buffer
  end

  def test_push_object_members
    @b.push_object.push_value(1, 'a').push_value(2, 'b').pop
    assert_equal '{"a":1,"b":2}', @b.buffer
  end

  def test_push_array_elements
    @b.push_array.push_value(1).push_value(2).push_value(3).pop
    assert_equal '[1,2,3]', @b.buffer
  end

  def test_push_symbol_key
    @b.push_object.push_value(1, :a).pop
    assert_equal '{"a":1}', @b.buffer
  end

  def test_push_key_then_value
    @b.push_object.push_key('a').push_value(1).pop
    assert_equal '{"a":1}', @b.buffer
  end

  def test_push_nested_array_in_object
    @b.push_object.push_array('list').push_value(1).push_value(2).pop.pop
    assert_equal '{"list":[1,2]}', @b.buffer
  end

  def test_push_nested_object_in_object
    @b.push_object.push_object('meta').push_value(true, 'ok').pop.pop
    assert_equal '{"meta":{"ok":true}}', @b.buffer
  end

  def test_push_value_accepts_whole_structures
    @b.push_array.push_value({ 'a' => 1 }).push_value([2, 3]).pop
    assert_equal '[{"a":1},[2,3]]', @b.buffer
  end

  def test_pop_all_closes_everything
    @b.push_object.push_array('a').push_value(1)
    @b.pop_all
    assert_equal '{"a":[1]}', @b.buffer
  end

  def test_writer_round_trips_a_complex_document
    @b.push_object
    @b.push_value('Toyota', 'make')
    @b.push_value(2017, 'year')
    @b.push_array('tires')
    [30.0, 30.2, 30.5].each { |t| @b.push_value(t) }
    @b.pop
    @b.pop
    assert_equal({ 'make' => 'Toyota', 'year' => 2017, 'tires' => [30.0, 30.2, 30.5] },
                 Simdjson.parse(@b.buffer))
  end

  def test_push_methods_return_self
    assert_same @b, @b.push_array
    assert_same @b, @b.push_value(1)
    assert_same @b, @b.pop
  end

  # --- Writer: misuse is reported, not silently mis-serialized ---

  def test_object_member_requires_a_key
    @b.push_object
    assert_raises(Simdjson::BuilderError) { @b.push_value(1) }
  end

  def test_array_element_rejects_a_key
    @b.push_array
    assert_raises(Simdjson::BuilderError) { @b.push_value(1, 'a') }
  end

  def test_pop_with_nothing_open_raises
    assert_raises(Simdjson::BuilderError) { @b.pop }
  end

  def test_push_key_outside_object_raises
    @b.push_array
    assert_raises(Simdjson::BuilderError) { @b.push_key('a') }
  end

  def test_push_key_twice_raises
    @b.push_object.push_key('a')
    assert_raises(Simdjson::BuilderError) { @b.push_key('b') }
  end

  def test_pop_after_push_key_without_value_raises
    @b.push_object.push_key('a')
    assert_raises(Simdjson::BuilderError) { @b.pop }
  end

  # --- Buffer / lifecycle ---

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

  def test_clear_discards_open_containers
    @b.push_object.push_key('a').clear
    assert_equal '[]', @b.push_array.pop.buffer
  end

  def test_to_s_is_buffer
    @b.append('x')
    assert_equal @b.buffer, @b.to_s
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

  # --- Streaming (io: / buffer_size: / flush) ---

  def test_streams_to_io_as_the_buffer_fills
    io = StringIO.new
    b = Simdjson::Builder.new(io: io, buffer_size: 16)
    b.push_array
    10.times { |i| b.push_value("element-#{i}") }
    # Past the 16-byte buffer, content has already been handed to the io before
    # we ask for a flush -- i.e. it streamed rather than buffering the whole array.
    refute_empty io.string, 'expected bytes to reach the io before #flush'
    b.pop
    b.flush
    assert_equal((0...10).map { |i| "element-#{i}" }, Simdjson.parse(io.string))
  end

  def test_flush_writes_the_remaining_bytes
    io = StringIO.new
    b = Simdjson::Builder.new(io: io, buffer_size: 1 << 20) # large: nothing auto-flushes
    b.push_object.push_value(1, 'a').pop
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

  def test_structure_survives_a_mid_document_flush
    io = StringIO.new
    b = Simdjson::Builder.new(io: io, buffer_size: 8)
    b.push_object
    b.push_value('a-fairly-long-value', 'key') # forces a flush mid-object
    b.push_value(2, 'b')
    b.pop
    b.flush
    assert_equal({ 'key' => 'a-fairly-long-value', 'b' => 2 }, Simdjson.parse(io.string))
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
