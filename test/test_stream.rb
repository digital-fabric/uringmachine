# frozen_string_literal: true

require_relative 'helper'
require 'securerandom'

class StreamBaseTest < UMBaseTest
  def setup
    super
    @rfd, @wfd = UM.pipe
    @stream = UM::Stream.new(@machine, @rfd)
  end

  def teardown
    machine.close(@rfd) rescue nil
    machine.close(@wfd) rescue nil
  end
end

class StreamTest < StreamBaseTest
  def buffer_metrics
    p machine.metrics
    machine.metrics.fetch_values(
      :buffer_groups,
      :buffers_allocated,
      :buffers_free,
      :segments_free,
      :buffer_space_allocated,
      :buffer_space_commited,
    )
  end

  def test_stream_basic_usage
    rfd, wfd = UM.pipe
    stream = UM::Stream.new(machine, rfd)

    assert_equal [0, 0, 0, 0, 0, 0], buffer_metrics
    machine.write(wfd, "foobar")
    machine.close(wfd)
    
    buf = stream.get_string(3)
    assert_equal 'foo', buf

    buf = stream.get_string(-6)
    assert_equal 'bar', buf
    assert stream.eof?

    stream = nil
    GC.start
    assert_equal [1, 4, 0, 256, 65536 * 4, 65536 * 4 - 6], buffer_metrics
  ensure
    machine.close(rfd) rescue nil
    machine.close(wfd) rescue nil
  end

  def test_stream_big_read
    s1, s2 = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)
    stream = UM::Stream.new(machine, s2)
    
    msg = '1234567' * 20000

    f = machine.spin {
      machine.sendv(s1, msg)
      machine.shutdown(s1, UM::SHUT_WR)
    }

    buf = stream.get_string(msg.bytesize)
    assert_equal msg, buf
  ensure
    machine.terminate(f)
    machine.join(f)
  end

  def test_stream_closed
    machine.write(@wfd, "foo\nbar\r\nbaz")
    machine.close(@wfd)
    s = nil
    machine.stream_read(@rfd) do |stream|
      s = stream
    end
    assert_raises(UM::Error) { s.get_line(0) }
  end

  def test_stream_get_line
    machine.write(@wfd, "foo\nbar\r\nbaz")
    machine.close(@wfd)

    assert_equal [0, 0, 0], buffer_metrics

    machine.stream_read(@rfd) do |stream|
      machine.snooze
      assert_equal [0, 0, 0], buffer_metrics

      assert_kind_of UM::Stream, stream
      assert_equal 'foo', stream.get_line(0)

      assert_equal [255, 1, 64], buffer_metrics
      assert_equal 'bar', stream.get_line(0)
      assert_nil stream.get_line(0)

      assert_equal [255, 1, 64], buffer_metrics
    end
    assert_equal [256, 1, 64], buffer_metrics
  end

  def test_stream_get_line_segmented
    machine.stream_read(@rfd) do |stream|
      assert_kind_of UM::Stream, stream

      machine.write(@wfd, "foo\n")
      assert_equal 'foo', stream.get_line(0)

      machine.write(@wfd, "bar")
      machine.write(@wfd, "\r\n")
      machine.write(@wfd, "baz\n")
      machine.close(@wfd)

      assert_equal [253, 1, 64], buffer_metrics
      assert_equal 'bar', stream.get_line(0)
      assert_equal [255, 1, 64], buffer_metrics
      assert_equal 'baz', stream.get_line(0)
      assert_equal [256, 1, 64], buffer_metrics
      assert_nil stream.get_line(0)
    end
    assert_equal [256, 1, 64], buffer_metrics
  end

  def test_stream_get_line_maxlen
    machine.write(@wfd, "foobar\r\n")

    machine.stream_read(@rfd) do |stream|
      assert_nil stream.get_line(3)
      # verify that stream pos has not changed
      assert_equal 'foobar', stream.get_line(0)

      machine.write(@wfd, "baz")
      machine.write(@wfd, "\n")
      machine.write(@wfd, "bizz")
      machine.write(@wfd, "\n")
      machine.close(@wfd)

      assert_nil stream.get_line(2)
      assert_nil stream.get_line(3)
      assert_equal 'baz', stream.get_line(4)

      assert_nil stream.get_line(3)
      assert_nil stream.get_line(4)
      assert_equal 'bizz', stream.get_line(5)

      assert_nil stream.get_line(8)
    end
    assert_equal [256, 1, 64], buffer_metrics
  end

  def test_stream_get_string
    machine.write(@wfd, "foobarbazblahzzz")
    machine.close(@wfd)

    machine.stream_read(@rfd) do |stream|
      assert_equal 'foobar', stream.get_string(6)
      assert_equal 'baz', stream.get_string(3)
      assert_equal 'blah', stream.get_string(4)
      assert_nil stream.get_string(4)
    end
  end

  def test_stream_get_string_zero_len
    machine.write(@wfd, "foobar")

    machine.stream_read(@rfd) do |stream|
      assert_equal 'foobar', stream.get_string(0)

      machine.write(@wfd, "bazblah")
      machine.close(@wfd)
      assert_equal 'bazblah', stream.get_string(0)
      assert_nil stream.get_string(0)
    end
  end

  def test_stream_get_string_negative_len
    machine.write(@wfd, "foobar")

    machine.stream_read(@rfd) do |stream|
      assert_equal 'foo', stream.get_string(-3)
      assert_equal 'bar', stream.get_string(-6)

      machine.write(@wfd, "bazblah")
      machine.close(@wfd)
      assert_equal 'bazblah', stream.get_string(-12)
      assert_nil stream.get_string(-3)
    end
  end

  def test_stream_big_data
    data = SecureRandom.random_bytes(300_000)
    fiber = machine.spin {
      machine.writev(@wfd, data)
      machine.close(@wfd)
    }

    received = []
    machine.stream_read(@rfd) do |stream|
      loop {
        msg = stream.get_string(-60_000)
        break if !msg

        received << msg
      }
    end
    machine.join(fiber)
    # since a pipe is limited to 64KB, we're going to receive 4 pairs of 60000B
    # and 5536B, then the remainder
    assert_equal 9, received.size
    assert_equal data, received.join
  end
end

class StreamRespTest < StreamBaseTest
  def test_stream_resp_decode
    machine.stream_read(@rfd) do |stream|
      machine.write(@wfd, "+foo bar\r\n")
      assert_equal "foo bar", stream.resp_decode

      machine.write(@wfd, "+baz\r\n")
      assert_equal "baz", stream.resp_decode

      machine.write(@wfd, "-foobar\r\n")
      o = stream.resp_decode
      assert_kind_of UM::Stream::RESPError, o
      assert_equal "foobar", o.message

      machine.write(@wfd, "!3\r\nbaz\r\n")
      o = stream.resp_decode
      assert_kind_of UM::Stream::RESPError, o
      assert_equal "baz", o.message

      machine.write(@wfd, ":123\r\n")
      assert_equal 123, stream.resp_decode

      machine.write(@wfd, ":-123\r\n")
      assert_equal(-123, stream.resp_decode)

      machine.write(@wfd, ",123.321\r\n")
      assert_equal 123.321, stream.resp_decode

      machine.write(@wfd, "_\r\n")
      assert_nil stream.resp_decode

      machine.write(@wfd, "#t\r\n")
      assert_equal true, stream.resp_decode

      machine.write(@wfd, "#f\r\n")
      assert_equal false, stream.resp_decode

      machine.write(@wfd, "$6\r\nfoobar\r\n")
      assert_equal "foobar", stream.resp_decode

      machine.write(@wfd, "$3\r\nbaz\r\n")
      assert_equal "baz", stream.resp_decode

      machine.write(@wfd, "=10\r\ntxt:foobar\r\n")
      assert_equal "foobar", stream.resp_decode

      machine.write(@wfd, "*3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
      assert_equal ['foo', 42, 'bar'], stream.resp_decode

      machine.write(@wfd, "~3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
      assert_equal ['foo', 42, 'bar'], stream.resp_decode

      machine.write(@wfd, ">3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
      assert_equal ['foo', 42, 'bar'], stream.resp_decode

      machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n:43\r\n")
      assert_equal({ 'a' => 42, 'b' => 43 }, stream.resp_decode)

      machine.write(@wfd, "|2\r\n+a\r\n:42\r\n+b\r\n:43\r\n")
      assert_equal({ 'a' => 42, 'b' => 43 }, stream.resp_decode)

      machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n*3\r\n+foo\r\n+bar\r\n+baz\r\n")
      assert_equal({ 'a' => 42, 'b' => ['foo', 'bar', 'baz'] }, stream.resp_decode)
    end
  end

  def test_stream_resp_decode_segmented
    machine.stream_read(@rfd) do |stream|
      machine.write(@wfd, "\n")
      assert_equal "", stream.get_line(0)

      machine.write(@wfd, "+foo")
      machine.write(@wfd, " ")
      machine.write(@wfd, "bar\r")
      machine.write(@wfd, "\n")
      assert_equal "foo bar", stream.resp_decode
      machine.write(@wfd, "$6\r")
      machine.write(@wfd, "\nbazbug")
      machine.write(@wfd, "\r\n")
      assert_equal "bazbug", stream.resp_decode
    end
  end

  def test_stream_resp_encode
    s = UM::Stream
    assert_equal "_\r\n",             s.resp_encode(+'', nil)
    assert_equal "#t\r\n",            s.resp_encode(+'', true)
    assert_equal "#f\r\n",            s.resp_encode(+'', false)
    assert_equal ":42\r\n",           s.resp_encode(+'', 42)
    assert_equal ",42.1\r\n",         s.resp_encode(+'', 42.1)
    assert_equal "$6\r\nfoobar\r\n",  s.resp_encode(+'', 'foobar')
    assert_equal "$10\r\nפובאר\r\n",   s.resp_encode(+'', 'פובאר')

    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n",
      s.resp_encode(+'', ['foo', 'bar'])

    assert_equal "%2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$3\r\nbaz\r\n:42\r\n",
      s.resp_encode(+'', { 'foo' => 'bar', 'baz' => 42 })
  end
end

class StreamStressTest < UMBaseTest
  def setup
    super

    @port = assign_port
    @listen_fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    machine.setsockopt(@listen_fd, UM::SOL_SOCKET, UM::SO_REUSEADDR, true)
    machine.setsockopt(@listen_fd, UM::SOL_SOCKET, UM::SO_REUSEPORT, true)
    machine.bind(@listen_fd, '127.0.0.1', @port)
    machine.listen(@listen_fd, 128)

    @received = []
    @response_body = "Hello, world!"
    @response_headers = "HTTP/1.1 200\r\nContent-Length: #{@response_body.bytesize}\r\n\r\n"
  end

  def start_connection_fiber(fd)
    machine.spin do
      machine.stream_recv(fd) { |stream|
        while (msg = stream.get_line(0))
          @received << msg
          if msg.empty?
            machine.sendv(fd, @response_headers, @response_body)
          end
        end
      }
      machine.close(fd)
    rescue => e
      p e
      p e.backtrace
    end
  end

  def test_stream_server_big_lines
    server_fibers = []
    server_fibers << machine.spin do
      machine.accept_each(@listen_fd) { |fd|
        server_fibers << start_connection_fiber(fd)
      }
    rescue Errno::EINVAL
      # ignore
    rescue => e
      p e
      p e.backtrace
    end

    client_count = 100
    msg_count = 100
    length = 1000
    total_msgs = client_count * msg_count
    msg = "#{SecureRandom.hex(length / 2)}\n" * msg_count
    client_fibers = client_count.times.map {
      machine.spin do
        fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
        machine.connect(fd, '127.0.0.1', @port)
        machine.send(fd, msg, msg.bytesize, UM::MSG_WAITALL)
        machine.close(fd)
      rescue => e
        p e
        p e.backtrace
      end
    }

    machine.await_fibers(client_fibers)
    machine.shutdown(@listen_fd, UM::SHUT_RD)
    machine.await_fibers(server_fibers)
    machine.snooze
    # assert_equal total_msgs, @received.size
    assert_equal msg * client_count, @received.map { it + "\n" }.join
  end

  def test_stream_server_http
    server_fibers = []
    server_fibers << machine.spin do
      machine.accept_each(@listen_fd) { |fd|
        server_fibers << start_connection_fiber(fd)
      }
    rescue Errno::EINVAL
      # ignore
    rescue => e
      p e
      p e.backtrace
    end

    client_count = 4
    msg_count = 10000
    total_msgs = client_count * msg_count
    msg = "GET http://127.0.0.1:1234/ HTTP/1.1\r\nhost: 127.0.0.1\r\n\r\n"
    client_fibers = client_count.times.map {
      machine.spin do
        fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
        machine.connect(fd, '127.0.0.1', @port)
        msg_count.times {
          machine.send(fd, msg, msg.bytesize, UM::MSG_WAITALL)
        }
        machine.close(fd)
      rescue => e
        p e
        p e.backtrace
      end
    }

    machine.await_fibers(client_fibers)
    machine.shutdown(@listen_fd, UM::SHUT_RD)
    machine.await_fibers(server_fibers)
    machine.snooze
    # assert_equal total_msgs, @received.size
    assert_equal msg * msg_count * client_count, @received.map { it + "\r\n" }.join
  end
end
