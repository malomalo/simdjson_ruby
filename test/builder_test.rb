# frozen_string_literal: true

require 'test_helper'

class BuilderTest < Minitest::Test
  def setup
    @b = Simdjson::Builder.new
  end

  def test_empty_object
    @b.start_object.end_object
    assert_equal '{}', @b.view
  end

  def test_empty_array
    @b.start_array.end_array
    assert_equal '[]', @b.view
  end

  SCALARS = { 42 => '42', -7 => '-7', true => 'true', false => 'false',
              nil => 'null', 1.5 => '1.5', 'hi' => '"hi"' }.freeze

  def test_scalars
    SCALARS.each { |value, expected| assert_equal expected, @b.clear.append(value).view }
  end

  def test_symbol_is_appended_as_string
    assert_equal '"foo"', @b.append(:foo).view
  end

  def test_bignum_serialized_as_number
    big = 123_456_789_012_345_678_901_234_567_890
    assert_equal big.to_s, @b.append(big).view
  end

  def test_string_escaping
    @b.append("a \"quote\" and \\ and\n newline")
    assert_equal %("a \\"quote\\" and \\\\ and\\n newline"), @b.view
  end

  def test_unicode_is_preserved
    @b.append('café')
    assert_equal '"café"', @b.view
    assert @b.validate_unicode
  end

  def test_append_transcodes_non_utf8_string_to_utf8
    latin1 = 'café'.encode('ISO-8859-1')
    @b.append(latin1)
    assert_equal '"café"', @b.view
    assert_equal Encoding::UTF_8, @b.view.encoding
    assert @b.validate_unicode
  end

  def test_append_key_transcodes_non_utf8_string
    @b.start_object.append_key('café'.encode('ISO-8859-1')).append_colon.append(1).end_object
    assert_equal({ 'café' => 1 }, Simdjson.parse(@b.view))
  end

  def test_append_rejects_untranscodable_bytes
    assert_raises(EncodingError) { @b.append("\xFF".b) }
  end

  def test_view_is_utf8
    assert_equal Encoding::UTF_8, @b.append('x').view.encoding
  end

  def test_append_raw_is_not_escaped
    @b.append_raw('[1,2,3]')
    assert_equal '[1,2,3]', @b.view
  end

  def test_append_key_and_colon
    @b.start_object.append_key('name').append_colon.append('Ada').end_object
    assert_equal '{"name":"Ada"}', @b.view
  end

  def test_append_key_value
    @b.start_object.append_key_value('year', 2017).end_object
    assert_equal '{"year":2017}', @b.view
  end

  def test_key_value_escapes_key
    @b.append_key_value('a"b', 1)
    assert_equal '"a\\"b":1', @b.view
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
                 Simdjson.parse(@b.view))
  end

  def test_size_tracks_written_bytes
    assert_equal 0, @b.size
    @b.append('ab') # => "ab" (4 bytes with quotes)
    assert_equal 4, @b.size
  end

  def test_clear_resets_and_allows_reuse
    @b.append(1).clear
    assert_equal 0, @b.size
    assert_equal '2', @b.append(2).view
  end

  def test_initial_capacity_argument
    b = Simdjson::Builder.new(8)
    b.append('a longer string than eight bytes')
    assert_equal '"a longer string than eight bytes"', b.view
  end

  def test_to_s_is_view
    @b.append('x')
    assert_equal @b.view, @b.to_s
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

  def test_chaining_returns_self
    assert_same @b, @b.start_array
    assert_same @b, @b.append(1)
    assert_same @b, @b.end_array
  end
end
