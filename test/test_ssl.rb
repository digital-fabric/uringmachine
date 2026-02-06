# frozen_string_literal: true

require_relative 'helper'
require 'openssl'
require 'localhost/authority'

class SSLTest < UMBaseTest
  def setup
    super
    authority = Localhost::Authority.fetch
    @server_ctx = authority.server_context
  end

  def test_ssl_stock
    sock1, sock2 = UNIXSocket.pair

    s1 = OpenSSL::SSL::SSLSocket.new(sock1, @server_ctx)
    s1.sync_close = true
    s2 = OpenSSL::SSL::SSLSocket.new(sock2, OpenSSL::SSL::SSLContext.new)
    s2.sync_close = true

    t = Thread.new { s1.accept rescue nil }

    s2.connect

    assert_equal 6, s1.write('foobar')
    assert_equal 'foobar', s2.readpartial(6)
  ensure
    t.join(0.1)
    sock1&.close rescue nil
    sock2&.close rescue nil
  end

  def test_ssl_custom_bio
    sock1, sock2 = UNIXSocket.pair

    s1 = OpenSSL::SSL::SSLSocket.new(sock1, @server_ctx)
    s1.sync_close = true
    s2 = OpenSSL::SSL::SSLSocket.new(sock2, OpenSSL::SSL::SSLContext.new)
    s2.sync_close = true

    @machine.ssl_set_bio(s2)
    assert_equal true, s2.instance_variable_get(:@__um_bio__)

    t = Thread.new { s1.accept rescue nil }

    assert_equal 0, @machine.metrics[:total_ops]
    s2.connect
    refute_equal 0, @machine.metrics[:total_ops]

    assert_equal 6, s1.write('foobar')
    assert_equal 'foobar', s2.readpartial(6)
  ensure
    t&.join(0.1)
    sock1&.close rescue nil
    sock2&.close rescue nil
  end

  def test_ssl_read_write
    sock1, sock2 = UNIXSocket.pair

    s1 = OpenSSL::SSL::SSLSocket.new(sock1, @server_ctx)
    s1.sync_close = true
    s2 = OpenSSL::SSL::SSLSocket.new(sock2, OpenSSL::SSL::SSLContext.new)
    s2.sync_close = true

    @machine.ssl_set_bio(s2)
    assert_equal true, s2.instance_variable_get(:@__um_bio__)

    t = Thread.new { s1.accept rescue nil }

    assert_equal 0, @machine.metrics[:total_ops]
    s2.connect
    refute_equal 0, @machine.metrics[:total_ops]

    assert_equal 6, @machine.ssl_write(s1, 'foobar', 6)
    buf = +''
    assert_equal 6, @machine.ssl_read(s2, buf, 6)
    assert_equal 'foobar', buf
  ensure
    t&.join(0.1)
    sock1&.close rescue nil
    sock2&.close rescue nil
  end
end
