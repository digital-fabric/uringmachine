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

@ssl_stock = OpenSSL::SSL::SSLSocket.open("127.0.0.1", port)
@ssl_stock.sync_close = true
@ssl_stock.connect

um = UM.new

@ssl_um = OpenSSL::SSL::SSLSocket.open("127.0.0.1", port)
@ssl_um.sync_close = true
um.ssl_set_bio(@ssl_um)
@ssl_um.connect

@msg = 'abc' * 1000

def do_io(ssl)
  ssl.puts @msg
  ssl.gets
end

Benchmark.ips do |x|
  x.report('stock') { do_io(@ssl_stock) }
  x.report('UM BIO') { do_io(@ssl_um) }

  x.compare!(order: :baseline)
end
