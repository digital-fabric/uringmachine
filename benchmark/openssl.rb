# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'benchmark-ips'
  gem 'openssl'
  gem 'localhost'
end

require 'uringmachine'
require 'benchmark/ips'
require 'openssl'
require 'localhost/authority'

authority = Localhost::Authority.fetch
ctx = authority.server_context
ctx.verify_mode = OpenSSL::SSL::VERIFY_NONE

Socket.do_not_reverse_lookup = true
tcps = TCPServer.new("127.0.0.1", 0)
port = tcps.connect_address.ip_port

pid = fork do
  ssls = OpenSSL::SSL::SSLServer.new(tcps, ctx)

  Thread.new do
    Thread.current.report_on_exception = false
    loop do
      begin
        ssl = ssls.accept
      rescue OpenSSL::SSL::SSLError, IOError, Errno::EBADF, Errno::EINVAL,
             Errno::ECONNABORTED, Errno::ENOTSOCK, Errno::ECONNRESET
        retry
      end

      Thread.new do
        Thread.current.report_on_exception = false

        begin
          while line = ssl.gets
            ssl.write(line)
          end
        ensure
          ssl.close
        end
        true
      end
    end
  end
  sleep
rescue Interrupt
end

at_exit {
  Process.kill('SIGINT', pid)
  Process.wait(pid)
}

begin
  @ssl_stock = OpenSSL::SSL::SSLSocket.open("127.0.0.1", port)
rescue => e
  p e
  exit!
end
@ssl_stock.sync_close = true
@ssl_stock.connect

@um = UM.new

@ssl_um = OpenSSL::SSL::SSLSocket.open("127.0.0.1", port)
@ssl_um.sync_close = true
@um.ssl_set_bio(@ssl_um)
@ssl_um.connect

@ssl_stream = OpenSSL::SSL::SSLSocket.open("127.0.0.1", port)
@ssl_stream.sync_close = true
@um.ssl_set_bio(@ssl_stream)
@ssl_stream.connect

@stream = @um.stream(@ssl_stream, :ssl)

@msg = 'abc' * 1000
@msg_newline = @msg + "\n"

def do_io(ssl)
  ssl.puts @msg
  ssl.gets
end

def do_io_stream(ssl, um, stream)
  um.ssl_write(ssl, @msg_newline, 0)
  stream.get_line(0)
end

Benchmark.ips do |x|
  x.report('stock') { do_io(@ssl_stock) }
  x.report('UM BIO') { do_io(@ssl_um) }
  x.report('UM Stream') { do_io_stream(@ssl_stream, @um, @stream) }

  x.compare!(order: :baseline)
end
