# frozen_string_literal: true

require_relative 'helper'

class StreamBaseTest < UMBaseTest
  def setup
    super
    @rfd, @wfd = UM.pipe
    @stream = UM::Stream.new(@machine, @rfd)
  end  
end

class StreamTest < StreamBaseTest
  def test_get_line
    machine.write(@wfd, "foo\nbar\r\nbaz")
    machine.close(@wfd)

    assert_equal 'foo', @stream.get_line
    assert_equal 'bar', @stream.get_line
    assert_nil @stream.get_line
  end

  def test_get_string
    machine.write(@wfd, "foobarbazblahzzz")
    machine.close(@wfd)

    assert_equal 'foobar', @stream.get_string(6)
    assert_equal 'baz', @stream.get_string(3)
    assert_equal 'blah', @stream.get_string(4)
    assert_nil @stream.get_string(4)
  end
end

class StreamRespTest < StreamBaseTest
  def test_trdp_get_line
    machine.write(@wfd, "foo\r\nbarbar\r\nbaz\n")
    machine.close(@wfd)
    
    assert_equal 'foo', @stream.resp_get_line
    assert_equal 'barbar', @stream.resp_get_line
    assert_nil @stream.resp_get_line
  end

  def test_resp_get_string
    machine.write(@wfd, "foo\r\nbarbar\r\nbaz\n")
    machine.close(@wfd)

    assert_equal 'foo', @stream.resp_get_string(3)
    assert_equal 'barbar', @stream.resp_get_string(6)
    assert_nil @stream.resp_get_string(3)
  end

  def test_resp_decode
    machine.write(@wfd, "+OK\r\n")
    assert_equal "OK", @stream.resp_decode
  end
end
