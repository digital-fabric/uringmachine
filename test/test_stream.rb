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

    assert_equal 'foo', @stream.get_line(nil, 0)
    assert_equal 'bar', @stream.get_line(nil, 0)
    assert_nil @stream.get_line(nil, 0)
  end

  def test_get_line_with_buf
    machine.write(@wfd, "foo\nbar\r\nbaz")
    machine.close(@wfd)

    buf = +''
    ret = @stream.get_line(buf, 0)
    assert_equal 'foo', buf
    assert_equal ret, buf

    ret = @stream.get_line(buf, 0)
    assert_equal 'bar', buf
    assert_equal ret, buf
  end

  def test_get_line_with_positive_maxlen
    machine.write(@wfd, "foobar\r\n")
    machine.close(@wfd)

    buf = +''
    ret = @stream.get_line(buf, 3)
    assert_nil ret
    assert_equal '', buf

    # verify that stream pos has not changed
    ret = @stream.get_line(buf, 0)
    assert_equal 'foobar', buf
    assert_equal ret, buf
  end

  def test_get_line_with_negative_maxlen
    machine.write(@wfd, "foobar\r\n")
    machine.close(@wfd)

    buf = +''
    ret = @stream.get_line(buf, -3)
    assert_nil ret
    assert_equal '', buf

    # verify that stream pos has not changed
    ret = @stream.get_line(buf, 0)
    assert_equal 'foobar', buf
    assert_equal ret, buf
  end

  def test_get_string
    machine.write(@wfd, "foobarbazblahzzz")
    machine.close(@wfd)

    assert_equal 'foobar', @stream.get_string(nil, 6)
    assert_equal 'baz', @stream.get_string(nil, 3)
    assert_equal 'blah', @stream.get_string(nil, 4)
    assert_nil @stream.get_string(nil, 4)
  end

  def test_get_string_with_buf
    machine.write(@wfd, "foobarbazblahzzz")
    machine.close(@wfd)

    buf = +''
    ret = @stream.get_string(buf, 6)
    assert_equal 'foobar', buf
    assert_equal ret, buf

    ret = @stream.get_string(buf, 3)
    assert_equal 'baz', buf
    assert_equal ret, buf
  end

  def test_get_string_with_negative_len
    machine.write(@wfd, "foobar")
    machine.close(@wfd)

    ret = @stream.get_string(nil, -12)
    assert_equal 'foobar', ret

    ret = @stream.get_string(nil, -4)
    assert_nil ret
  end

  def test_skip
    machine.write(@wfd, "foobar")
    machine.close(@wfd)

    ret = @stream.get_string(nil, 2)
    assert_equal 'fo', ret

    ret = @stream.skip(2)
    assert_equal 2, ret

    ret = @stream.get_string(nil, 2)
    assert_equal 'ar', ret

    ret = @stream.skip(2)
    assert_nil ret
  end
end

class StreamRespTest < StreamBaseTest
  def test_resp_decode
    machine.write(@wfd, "+foo bar\r\n")
    assert_equal "foo bar", @stream.resp_decode

    machine.write(@wfd, "+baz\r\n")
    assert_equal "baz", @stream.resp_decode

    machine.write(@wfd, "-foobar\r\n")
    o = @stream.resp_decode
    assert_kind_of UM::Stream::RESPError, o
    assert_equal "foobar", o.message

    machine.write(@wfd, "!3\r\nbaz\r\n")
    o = @stream.resp_decode
    assert_kind_of UM::Stream::RESPError, o
    assert_equal "baz", o.message

    machine.write(@wfd, ":123\r\n")
    assert_equal 123, @stream.resp_decode

    machine.write(@wfd, ":-123\r\n")
    assert_equal(-123, @stream.resp_decode)

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

    machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n*3\r\n+foo\r\n+xbar\r\n+yybaz\r\n")
    assert_equal({ 'a' => 42, 'b' => ['foo', 'xbar', 'yybaz'] }, @stream.resp_decode)
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

    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
      s.resp_encode(+'', ['foo', 'bar'])

    assert_equal "*2\r\n$3\r\nfoo\r\n$4\r\nxbar\r\n",
      s.resp_encode(+'', ['foo', 'xbar'])

    assert_equal "%2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$3\r\nbaz\r\n:42\r\n",
      s.resp_encode(+'', { 'foo' => 'bar', 'baz' => 42 })
  end

  def test_resp_encode_cmd
    s = UM::Stream

    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
      s.resp_encode_cmd(+'', 'foo', 'bar')

    assert_equal "*2\r\n$3\r\nfoo\r\n$4\r\nxbar\r\n",
      s.resp_encode_cmd(+'', 'foo', 'xbar')

    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
      s.resp_encode_cmd(+'', 'foo', :bar)

    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\n123\r\n",
      s.resp_encode_cmd(+'', 'foo', 123)

    assert_equal "*4\r\n$3\r\nset\r\n$6\r\nfoobar\r\n$2\r\nnx\r\n$2\r\nxx\r\n",
      s.resp_encode_cmd(+'', :set, 'foobar', :nx, :xx)
  end
end
