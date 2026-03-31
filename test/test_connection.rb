# frozen_string_literal: true

require_relative 'helper'
require 'securerandom'
require 'openssl'
require 'localhost/authority'

class ConnectionBaseTest < UMBaseTest
  attr_reader :conn

  def setup
    super
    @rfd, @wfd = UM.pipe
    @conn = UM::Connection.new(@machine, @rfd)
  end

  def teardown
    @conn = nil
    machine.close(@rfd) rescue nil
    machine.close(@wfd) rescue nil
    super
  end
end

class ConnectionTest < ConnectionBaseTest
  def buffer_metrics
    machine.metrics.fetch_values(
      :buffers_allocated,
      :buffers_free,
      :segments_free,
      :buffer_space_allocated,
      :buffer_space_commited,
    )
  end

  def test_connection_basic_usage
    assert_equal [0, 0, 0, 0, 0], buffer_metrics
    machine.write(@wfd, "foobar")
    machine.close(@wfd)

    buf = conn.read(3)
    assert_equal 'foo', buf

    buf = conn.read(-6)
    assert_equal 'bar', buf
    assert conn.eof?

    conn.clear

    # initial buffer size: 6BKV, initial buffers commited: 16 (256KB)
    # (plus an additional buffer commited after first usage)
    assert_equal [16, 0, 256, 16384 * 16, 16384 * 16 - 6], buffer_metrics
    assert_equal 0, machine.metrics[:ops_pending]
  end

  def test_connection_clear
    rfd, wfd = UM.pipe
    conn = UM::Connection.new(machine, rfd)

    assert_equal [0, 0, 0, 0, 0], buffer_metrics
    machine.write(wfd, "foobar")

    buf = conn.read(3)
    assert_equal 'foo', buf

    assert_equal 1, machine.metrics[:ops_pending]
    assert_equal 255, machine.metrics[:segments_free]

    conn.clear
    machine.snooze
    assert_equal 0, machine.metrics[:ops_pending]
    assert_equal 256, machine.metrics[:segments_free]

    assert_equal [16, 0, 256, 16384 * 16, 16384 * 16 - 6], buffer_metrics
  ensure
    machine.close(rfd) rescue nil
    machine.close(wfd) rescue nil
  end

  def test_connection_big_read
    s1, s2 = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)
    conn = UM::Connection.new(machine, s2)

    msg = '1234567' * 20000

    f = machine.spin do
      machine.sendv(s1, msg)
      machine.snooze
      machine.shutdown(s1, UM::SHUT_WR)
    end

    buf = conn.read(msg.bytesize)
    assert_equal msg, buf
  ensure
    machine.terminate(f)
    machine.join(f)
  end

  def test_connection_buffer_reuse
    s1, s2 = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)
    conn = UM::Connection.new(machine, s2)

    msg = '1234567' * 20000

    f = machine.spin do
      machine.sendv(s1, msg, msg)
      machine.sleep(0.05)
      machine.sendv(s1, msg, msg)
      machine.shutdown(s1, UM::SHUT_WR)
    end

    buf = conn.read(msg.bytesize)
    assert_equal msg, buf

    buf = conn.read(msg.bytesize)
    assert_equal msg, buf

    buf = conn.read(msg.bytesize)
    assert_equal msg, buf

    buf = conn.read(msg.bytesize)
    assert_equal msg, buf

    conn.clear
    # numbers may vary with different kernel versions
    assert_in_range 24..32, machine.metrics[:buffers_allocated]
    assert_in_range 10..18, machine.metrics[:buffers_free]
    assert_equal 256, machine.metrics[:segments_free]
  ensure
    machine.terminate(f)
    machine.join(f)
  end

  def test_connection_read_line
    machine.write(@wfd, "foo\nbar\r\nbaz")
    machine.close(@wfd)

    assert_equal [0, 0, 0, 0, 0], buffer_metrics

    assert_equal 'foo', conn.read_line(0)

    assert_equal [16, 0, 255, 16384 * 16, 16384 * 16 - 12], buffer_metrics
    assert_equal 'bar', conn.read_line(0)
    assert_nil conn.read_line(0)
    assert_equal "baz", conn.read(-6)
  end

  def test_connection_read_line_segmented
    machine.write(@wfd, "foo\n")
    assert_equal 'foo', conn.read_line(0)

    machine.write(@wfd, "bar")
    machine.write(@wfd, "\r\n")
    machine.write(@wfd, "baz\n")
    machine.close(@wfd)

    # three segments received
    assert_equal [16, 0, 253, 16384 * 16, 16384 * 16 - 13], buffer_metrics
    assert_equal 'bar', conn.read_line(0)
    assert_equal [16, 0, 255, 16384 * 16, 16384 * 16 - 13], buffer_metrics
    assert_equal 'baz', conn.read_line(0)
    assert_equal [16, 0, 256, 16384 * 16, 16384 * 16 - 13], buffer_metrics
    assert_nil conn.read_line(0)
  end

  def test_connection_read_line_maxlen
    machine.write(@wfd, "foobar\r\n")

    assert_nil conn.read_line(3)
      # verify that connecvtion pos has not changed
    assert_equal 'foobar', conn.read_line(0)

    machine.write(@wfd, "baz")
    machine.write(@wfd, "\n")
    machine.write(@wfd, "bizz")
    machine.write(@wfd, "\n")
    machine.close(@wfd)

    assert_nil conn.read_line(2)
    assert_nil conn.read_line(3)
    assert_equal 'baz', conn.read_line(4)

    assert_nil conn.read_line(3)
    assert_nil conn.read_line(4)
    assert_equal 'bizz', conn.read_line(5)

    assert_nil conn.read_line(8)
    assert_equal [16, 0, 256, 16384 * 16, 16384 * 16 - 17], buffer_metrics
  end

  def test_connection_read
    machine.write(@wfd, "foobarbazblahzzz")
    machine.close(@wfd)

    assert_equal 'foobar', conn.read(6)
    assert_equal 'baz', conn.read(3)
    assert_equal 'blah', conn.read(4)
    assert_nil conn.read(4)
  end

  def test_connection_read_zero_len
    machine.write(@wfd, "foobar")

    assert_equal 'foobar', conn.read(0)

    machine.write(@wfd, "bazblah")
    machine.close(@wfd)
    assert_equal 'bazblah', conn.read(0)
    assert_nil conn.read(0)
  end

  def test_connection_read_negative_len
    machine.write(@wfd, "foobar")

    assert_equal 'foo', conn.read(-3)
    assert_equal 'bar', conn.read(-6)

    machine.write(@wfd, "bazblah")
    machine.close(@wfd)
    assert_equal 'bazblah', conn.read(-12)
    assert_nil conn.read(-3)
  end

  def test_connection_read_to_delim
    machine.write(@wfd, "abc,def,ghi")
    machine.close(@wfd)

    assert_nil conn.read_to_delim('!', 0) # not there
    assert_nil conn.read_to_delim(',', 2) # too long
    assert_equal 'abc', conn.read_to_delim(',', 0)
    assert_equal 'def', conn.read_to_delim(',', 0)
    assert_nil conn.read_to_delim(',', 0)
    assert_equal 'ghi', conn.read_to_delim(',', -3)
  end

  def test_connection_read_to_delim_invalid_delim
    machine.write(@wfd, "abc,def,ghi")

    assert_raises(ArgumentError) { conn.read_to_delim(:foo, 0) }
    assert_raises(UM::Error) { conn.read_to_delim('', 0) }
    assert_raises(UM::Error) { conn.read_to_delim('ab', 0) }
    assert_raises(UM::Error) { conn.read_to_delim('🙂', 0) }
  end

  def test_connection_skip
    machine.write(@wfd, "foobarbaz")

    conn.skip(2)
    assert_equal 'obar', conn.read(4)

    conn.skip(1)
    assert_equal 'az', conn.read(0)
  end

  def test_connection_big_data
    data = SecureRandom.random_bytes(300_000)
    fiber = machine.spin {
      machine.writev(@wfd, data)
      machine.close(@wfd)
    }

    received = []
    loop {
      msg = conn.read(-60_000)
      break if !msg

      received << msg
    }
    machine.join(fiber)
    # since a pipe is limited to 64KB, we're going to receive 4 pairs of 60000B
    # and 5536B, then the remainder
    assert_equal 9, received.size
    assert_equal data, received.join
  end

  def test_connection_read_each
    bufs = []
    f = machine.spin do
      bufs << :ready
      conn.read_each {
        assert_kind_of IO::Buffer, it
        bufs << it.get_string
      }
      bufs << :done
    rescue => e
      p e
      p e.backtrace
    end

    machine.snooze
    assert_equal [:ready], bufs

    machine.write(@wfd, 'foo')
    machine.snooze
    assert_equal [:ready, 'foo'], bufs

    machine.write(@wfd, 'barb')
    machine.snooze
    assert_equal [:ready, 'foo', 'barb'], bufs

    machine.close(@wfd)
    machine.snooze
    assert_equal [:ready, 'foo', 'barb', :done], bufs
  ensure
    machine.terminate(f)
    machine.join(f)
  end
end

class ConnectionWriteTest < UMBaseTest
  attr_reader :conn

  def setup
    super
    @s1, @s2 = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)
    @conn = UM::Connection.new(@machine, @s1)
  end

  def teardown
    @conn = nil
    machine.close(@s1) rescue nil
    machine.close(@s2) rescue nil
    super
  end

  def test_connection_write_single_buf
    assert_equal 3, conn.write('foo')

    buf = +''
    machine.read(@s2, buf, 100)
    assert_equal 'foo', buf
  end

  def test_connection_write_multi_buf
    assert_equal 6, conn.write('foo', 'bar')

    buf = +''
    machine.read(@s2, buf, 100)
    assert_equal 'foobar', buf
  end

  def test_connection_write_socket_mode
    conn = machine.connection(@s2, :socket)

    assert_equal 6, conn.write('foo', 'bar')

    buf = +''
    machine.read(@s1, buf, 100)
    assert_equal 'foobar', buf
  end

  def test_connection_write_ssl_mode
    ssl1 = OpenSSL::SSL::SSLSocket.new(IO.for_fd(@s1), Localhost::Authority.fetch.server_context)
    ssl1.sync_close = true
    ssl2 = OpenSSL::SSL::SSLSocket.new(IO.for_fd(@s2), OpenSSL::SSL::SSLContext.new)
    ssl2.sync_close = true

    machine.ssl_set_bio(ssl1)
    machine.ssl_set_bio(ssl2)

    f = machine.spin { ssl1.accept rescue nil }

    ssl2.connect
    refute_equal 0, @machine.metrics[:total_ops]

    conn1 = machine.connection(ssl1)
    conn2 = machine.connection(ssl2)

    assert_equal 10, conn1.write('foobar', "\n", 'baz')

    assert_equal "foobar\nbaz", conn2.read(10)
  ensure
    ssl1.close rescue nil
    ss2.close rescue nil
    if f
      machine.terminate(f)
      machine.join(f)
    end
  end
end

class ConnectionRespTest < ConnectionBaseTest
  def test_connection_resp_read
    machine.write(@wfd, "+foo bar\r\n")
    assert_equal "foo bar", conn.resp_read

    machine.write(@wfd, "+baz\r\n")
    assert_equal "baz", conn.resp_read

    machine.write(@wfd, "-foobar\r\n")
    o = conn.resp_read
    assert_kind_of UM::Connection::RESPError, o
    assert_equal "foobar", o.message

    machine.write(@wfd, "!3\r\nbaz\r\n")
    o = conn.resp_read
    assert_kind_of UM::Connection::RESPError, o
    assert_equal "baz", o.message

    machine.write(@wfd, ":123\r\n")
    assert_equal 123, conn.resp_read

    machine.write(@wfd, ":-123\r\n")
    assert_equal(-123, conn.resp_read)

    machine.write(@wfd, ",123.321\r\n")
    assert_equal 123.321, conn.resp_read

    machine.write(@wfd, "_\r\n")
    assert_nil conn.resp_read

    machine.write(@wfd, "#t\r\n")
    assert_equal true, conn.resp_read

    machine.write(@wfd, "#f\r\n")
    assert_equal false, conn.resp_read

    machine.write(@wfd, "$6\r\nfoobar\r\n")
    assert_equal "foobar", conn.resp_read

    machine.write(@wfd, "$3\r\nbaz\r\n")
    assert_equal "baz", conn.resp_read

    machine.write(@wfd, "=10\r\ntxt:foobar\r\n")
    assert_equal "foobar", conn.resp_read

    machine.write(@wfd, "*3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
    assert_equal ['foo', 42, 'bar'], conn.resp_read

    machine.write(@wfd, "~3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
    assert_equal ['foo', 42, 'bar'], conn.resp_read

    machine.write(@wfd, ">3\r\n+foo\r\n:42\r\n$3\r\nbar\r\n")
    assert_equal ['foo', 42, 'bar'], conn.resp_read

    machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n:43\r\n")
    assert_equal({ 'a' => 42, 'b' => 43 }, conn.resp_read)

    machine.write(@wfd, "|2\r\n+a\r\n:42\r\n+b\r\n:43\r\n")
    assert_equal({ 'a' => 42, 'b' => 43 }, conn.resp_read)

    machine.write(@wfd, "%2\r\n+a\r\n:42\r\n+b\r\n*3\r\n+foo\r\n+bar\r\n+baz\r\n")
    assert_equal({ 'a' => 42, 'b' => ['foo', 'bar', 'baz'] }, conn.resp_read)
  end

  def test_connection_resp_read_segmented
    machine.write(@wfd, "\n")
    assert_equal "", conn.read_line(0)

    machine.write(@wfd, "+foo")
    machine.write(@wfd, " ")
    machine.write(@wfd, "bar\r")
    machine.write(@wfd, "\n")
    assert_equal "foo bar", conn.resp_read
    machine.write(@wfd, "$6\r")
    machine.write(@wfd, "\nbazbug")
    machine.write(@wfd, "\r\n")
    assert_equal "bazbug", conn.resp_read
  end

  def test_connection_resp_write
    writer = machine.connection(@wfd)

    writer.resp_write(nil);
    assert_equal "_\r\n", conn.read(-100)

    writer.resp_write(true);
    assert_equal "#t\r\n", conn.read(-100)
    
    writer.resp_write(false);
    assert_equal "#f\r\n", conn.read(-100)

    writer.resp_write(42);
    assert_equal ":42\r\n", conn.read(-100)

    writer.resp_write(42.1)
    assert_equal ",42.1\r\n", conn.read(-100)

    writer.resp_write('foobar')
    assert_equal "$6\r\nfoobar\r\n", conn.read(-100)

    writer.resp_write('פובאר')
    assert_equal (+"$10\r\nפובאר\r\n").force_encoding('ASCII-8BIT'), conn.read(-100)

    writer.resp_write(['foo', 'bar'])
    assert_equal "*2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n", conn.read(-100)

    writer.resp_write({ 'foo' => 'bar', 'baz' => 42 })
    assert_equal "%2\r\n$3\r\nfoo\r\n$3\r\nbar\r\n$3\r\nbaz\r\n:42\r\n", conn.read(-100)
  end

  def test_connection_resp_encode
    s = UM::Connection
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

class ConnectionStressTest < UMBaseTest
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
      conn = UM::Connection.new(machine, fd)
      while (msg = conn.read_line(0))
        @received << msg
      end
      machine.sendv(fd, @response_headers, @response_body)
      machine.close(fd)
    rescue => e
      p e
      p e.backtrace
    end
  end

  def test_connection_server_big_lines
    server_fibers = []
    server_fibers << machine.spin do
      machine.accept_each(@listen_fd) { |fd|
        server_fibers << start_connection_fiber(fd)
      }
    rescue Errno::EINVAL
      ignore
    rescue => e
      p e
      p e.backtrace
    end

    client_count = 1000
    msg_count = 100
    length = 100

    total_msgs = client_count * msg_count
    msg = "#{SecureRandom.hex(length / 2)}\n" * msg_count
    client_fibers = client_count.times.map {
      machine.snooze
      machine.spin do
        fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
        machine.connect(fd, '127.0.0.1', @port)
        machine.sleep(0.05)
        machine.send(fd, msg, msg.bytesize, UM::MSG_WAITALL)
        machine.sleep(0.05)
        machine.close(fd)
      rescue => e
        p e
        p e.backtrace
      end
    }

    machine.await(client_fibers)
    machine.shutdown(@listen_fd, UM::SHUT_RD)
    machine.await(server_fibers)
    machine.snooze

    assert_equal total_msgs, @received.size
    assert_equal msg * client_count, @received.map { it + "\n" }.join
  end

  def test_connection_server_http
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

    client_count = 1000
    msg_count = 16
    msg = "GET http://127.0.0.1:1234/ HTTP/1.1\r\nhost: 127.0.0.1\r\n\r\n"
    client_fibers = client_count.times.map {
      machine.snooze
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

    machine.await(client_fibers)
    machine.shutdown(@listen_fd, UM::SHUT_RD)
    machine.await(server_fibers)
    machine.snooze
    # assert_equal total_msgs, @received.size
    assert_equal msg * msg_count * client_count, @received.map { it + "\r\n" }.join
  end
end

class ConnectionDevRandomTest < UMBaseTest
  def test_connection_dev_random_read_line
    fd = machine.open('/dev/random', UM::O_RDONLY)
    conn = UM::Connection.new(machine, fd)

    n = 100000
    lines = []
    n.times {
      lines << conn.read_line(0)
    }

    assert_equal n, lines.size
  ensure
    conn.clear rescue nil
    machine.close(fd) rescue nil
  end

  def read_line_do(n, acc)
    fd = @machine.open('/dev/random', UM::O_RDONLY)
    conn = UM::Connection.new(@machine, fd)
    n.times { acc << conn.read_line(0) }
  end

  def test_connection_dev_random_read_line_concurrent
    acc = []
    c = 1
    n = 100000
    ff = c.times.map {
      machine.spin { read_line_do(n, acc) }
    }
    machine.await(ff)
    assert_equal c * n, acc.size
  end

  def test_connection_dev_random_read
    fd = machine.open('/dev/random', UM::O_RDONLY)
    conn = UM::Connection.new(machine, fd)

    n = 256
    size = 65536 * 8
    count = 0
    # lines = []
    n.times {
      l = conn.read(size)
      refute_nil l
      assert_equal size, l.bytesize

      count += 1
    }

    assert_equal n, count
  ensure
    conn.clear rescue nil
  end
end

class ConnectionModeTest < UMBaseTest
  def test_connection_default_mode
    r, w = UM.pipe
    conn = UM::Connection.new(machine, r)
    assert_equal :fd, conn.mode
  ensure
    machine.close(r) rescue nil
    machine.close(w) rescue nil
  end

  def test_connection_default_mode_ssl
    authority = Localhost::Authority.fetch
    @server_ctx = authority.server_context
    sock1, sock2 = UNIXSocket.pair

    s1 = OpenSSL::SSL::SSLSocket.new(sock1, @server_ctx)
    conn = UM::Connection.new(machine, s1)
    assert_equal :ssl, conn.mode
  ensure
    sock1&.close rescue nil
    sock2&.close rescue nil
  end

  def test_connection_socket_mode_non_socket
    r, w = UM.pipe
    machine.write(w, 'foobar')
    machine.close(w)

    conn = UM::Connection.new(machine, r, :socket)
    assert_equal :socket, conn.mode
    # assert :socket, conn.mode
    assert_raises(Errno::ENOTSOCK) { conn.read(0) }
  ensure
    machine.close(r) rescue nil
    machine.close(w) rescue nil
  end

  def test_connection_socket_mode_socket
    r, w = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)
    machine.write(w, 'foobar')
    machine.close(w)

    conn = UM::Connection.new(machine, r, :socket)
    assert_equal :socket, conn.mode
    buf = conn.read(0)
    assert_equal 'foobar', buf
  ensure
    machine.close(r) rescue nil
    machine.close(w) rescue nil
  end

  def test_connection_ssl_mode
    authority = Localhost::Authority.fetch
    @server_ctx = authority.server_context
    sock1, sock2 = UNIXSocket.pair

    s1 = OpenSSL::SSL::SSLSocket.new(sock1, @server_ctx)
    s1.sync_close = true
    s2 = OpenSSL::SSL::SSLSocket.new(sock2, OpenSSL::SSL::SSLContext.new)
    s2.sync_close = true

    @machine.ssl_set_bio(s1)
    @machine.ssl_set_bio(s2)
    assert_equal true, s1.instance_variable_get(:@__um_bio__)
    assert_equal true, s2.instance_variable_get(:@__um_bio__)

    f = machine.spin { s1.accept rescue nil }

    s2.connect
    refute_equal 0, @machine.metrics[:total_ops]

    buf = "foobar\nbaz"
    assert_equal 10, @machine.ssl_write(s1, buf, buf.bytesize)
    buf = +''

    conn = UM::Connection.new(machine, s2, :ssl)
    assert_equal "foobar", conn.read_line(0)

    buf = "buh"
    @machine.ssl_write(s1, buf, buf.bytesize)

    assert_equal "baz", conn.read(0)
    assert_equal "buh", conn.read(0)

    s1.close

    assert_nil conn.read(0)
  rescue => e
    p e
    p e.backtrace
    exit!
  ensure
    machine.join(f)
    sock1&.close rescue nil
    sock2&.close rescue nil
  end
end

class ConnectionByteCountsTest < ConnectionBaseTest
  def test_connection_byte_counts
    machine.write(@wfd, "foobar")

    assert_equal 0, conn.consumed
    assert_equal 0, conn.pending

    buf = conn.read(2)
    assert_equal 'fo', buf
    assert_equal 2, conn.consumed
    assert_equal 4, conn.pending

    buf = conn.read(3)
    assert_equal 'oba', buf
    assert_equal 5, conn.consumed
    assert_equal 1, conn.pending

    machine.write(@wfd, "abc\ndef")
    machine.snooze
    assert_equal 5, conn.consumed
    assert_equal 1, conn.pending

    buf = conn.read_line(0)
    assert_equal 'rabc', buf
    assert_equal 10, conn.consumed
    assert_equal 3, conn.pending

    conn.clear
    assert_equal 10, conn.consumed
    assert_equal 0, conn.pending
  end
end
