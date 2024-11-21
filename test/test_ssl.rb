require_relative "helper"

__END__

require 'uringmachine/ssl'
require 'openssl'
require 'localhost'

class TestSSLContext < Minitest::Test

  if false
    def test_raises_with_invalid_keystore_file
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.keystore = "/no/such/keystore" }
      assert_equal("Keystore file '/no/such/keystore' does not exist", exception.message)
    end

    def test_raises_with_unreadable_keystore_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.keystore = "/unreadable/keystore" }
          assert_equal("Keystore file '/unreadable/keystore' is not readable", exception.message)
        end
      end
    end
  else
    def test_raises_with_invalid_key_file
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.key = "/no/such/key" }
      assert_equal("Key file '/no/such/key' does not exist", exception.message)
    end

    def test_raises_with_unreadable_key_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.key = "/unreadable/key" }
          assert_equal("Key file '/unreadable/key' is not readable", exception.message)
        end
      end
    end

    def test_raises_with_invalid_cert_file
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.cert = "/no/such/cert" }
      assert_equal("Cert file '/no/such/cert' does not exist", exception.message)
    end

    def test_raises_with_unreadable_cert_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.key = "/unreadable/cert" }
          assert_equal("Key file '/unreadable/cert' is not readable", exception.message)
        end
      end
    end

    def test_raises_with_invalid_key_pem
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.key_pem = nil }
      assert_equal("'key_pem' is not a String", exception.message)
    end

    def test_raises_with_unreadable_ca_file
      ctx = UM::SSL::Context.new

      File.stub(:exist?, true) do
        File.stub(:readable?, false) do
          exception = assert_raises(ArgumentError) { ctx.ca = "/unreadable/cert" }
          assert_equal("ca file '/unreadable/cert' is not readable", exception.message)
        end
      end
    end

    def test_raises_with_invalid_cert_pem
      ctx = UM::SSL::Context.new

      exception = assert_raises(ArgumentError) { ctx.cert_pem = nil }
      assert_equal("'cert_pem' is not a String", exception.message)
    end

    def test_raises_with_invalid_key_password_command
      ctx = UM::SSL::Context.new
      ctx.key_password_command = '/unreadable/decrypt_command'

      assert_raises(Errno::ENOENT) { ctx.key_password }
    end
  end
end

class TestSSLServer < UMBaseTest
  def setup
    super
    @port = assign_port
    @ssl_ctx = create_ssl_ctx
  end

  def create_ssl_ctx
    authority = Localhost::Authority.fetch
    authority.server_context
  end

  def test_ssl_accept
    server_fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    machine.bind(server_fd, '127.0.0.1', @port)
    res = machine.listen(server_fd, 5)
    assert_equal 0, res
    assert_equal 0, machine.pending_count

    fd = nil
    sock = nil

    reply = nil
    t = Thread.new do
      p 21
      sleep 0.1
      p 22
      sock = TCPSocket.new('127.0.0.1', @port)
      p 23
      sleep 0.1
      p 24
      client = OpenSSL::SSL::SSLSocket.new(sock)
      p 25
      client.connect
      p 26
      client.write('foobar')
      p 27
      reply = client.read(8192)
      p 28
    end

    p 11
    fd = machine.accept(server_fd)
    p 12
    sock = machine.ssl_accept(fd, @ssl_ctx)
    msg = +''
    p 13
    ret = sock.recv(msg, 8192)
    p 14
    sock.send("Hello: #{msg} (#{ret})", 0)
    p 15
    machine.close(fd)

    assert_equal 'Hello: foobar (6)', reply
  end
end
