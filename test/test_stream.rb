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
    machine.write(@wfd, "+foo bar\r\n")
    assert_equal "foo bar", @stream.resp_decode

    machine.write(@wfd, "+baz\r\n")
    assert_equal "baz", @stream.resp_decode

    machine.write(@wfd, "-foobar\r\n")
    o = @stream.resp_decode
    assert_kind_of RuntimeError, o
    assert_equal "foobar", o.message

    machine.write(@wfd, "!3\r\nbaz\r\n")
    o = @stream.resp_decode
    assert_kind_of RuntimeError, o
    assert_equal "baz", o.message

    machine.write(@wfd, ":123\r\n")
    assert_equal 123, @stream.resp_decode

    machine.write(@wfd, ":-123\r\n")
    assert_equal -123, @stream.resp_decode

    machine.write(@wfd, ",123.321\r\n")
    assert_equal 123.321, @stream.resp_decode

    machine.write(@wfd, "_\r\n")
    assert_nil @stream.resp_decode

    machine.write(@wfd, "#t\r\n")
    assert_equal true, @stream.resp_decode

    machine.write(@wfd, "#f\r\n")
    assert_equal false, @stream.resp_decode

    machine.write(@wfd, "$6\r\nfoobar\r\n")
    assert_equal "foobar", @stream.resp_decode

    machine.write(@wfd, "$3\r\nbaz\r\n")
    assert_equal "baz", @stream.resp_decode

    machine.write(@wfd, "=10\r\ntxt:foobar\r\n")
    assert_equal "foobar", @stream.resp_decode

    machine.write(@wfd, "*3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
    assert_equal ['foo', 42, 'bar'], @stream.resp_decode

    machine.write(@wfd, "~3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
    assert_equal ['foo', 42, 'bar'], @stream.resp_decode

    machine.write(@wfd, ">3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
    assert_equal ['foo', 42, 'bar'], @stream.resp_decode

    machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n:43\r\n")
    assert_equal({ 'a' => 42, 'b' => 43 }, @stream.resp_decode)

    machine.write(@wfd, "|2\r\n+a\r\n:42\r\n+b\r\n:43\r\n")
    assert_equal({ 'a' => 42, 'b' => 43 }, @stream.resp_decode)

    machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n*3\r\n+foo\r\n+bar\r\n+baz\r\n")
    assert_equal({ 'a' => 42, 'b' => ['foo', 'bar', 'baz'] }, @stream.resp_decode)

  end

  def test_resp_encode
    s = UM::Stream    
    assert_equal "_\r\n",             s.resp_encode(+'', nil)
    assert_equal "#t\r\n",            s.resp_encode(+'', true)
    assert_equal "#f\r\n",            s.resp_encode(+'', false)
    assert_equal ":42\r\n",           s.resp_encode(+'', 42)
    assert_equal ",42.1\r\n",         s.resp_encode(+'', 42.1)
    assert_equal "$6\r\nfoobar\r\n",  s.resp_encode(+'', 'foobar')
    assert_equal "$10\r\nפובאר\r\n",  s.resp_encode(+'', 'פובאר')
    
    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
      s.resp_encode(+'', ['foo', 'bar'])

    assert_equal "%2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$3\r\nbaz\r\n:42\r\n",
      s.resp_encode(+'', { 'foo' => 'bar', 'baz' => 42 })
  end
end
