# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'
require 'securerandom'

@machine = UM.new

@parts = ['1' * 256, '2' * (1 << 14), '3' * 256]
@total_len = @parts.map(&:bytesize).reduce(:+)

def io_write
  @server_io.write(*@parts)
end

def um_write
  str = @parts.join
  len = str.bytesize

  while len > 0
    ret = @machine.write(@s2, str, len)
    len -= ret
    str = str[ret..-1] if len > 0
  end
end

def um_writev
  @machine.writev(@s2, *@parts)
end

def um_send
  str = @parts.join
  @machine.send(@s2, str, str.bytesize, UM::MSG_WAITALL)
end

def um_sendv
  @machine.sendv(@s2, *@parts)
end

@bgid = @machine.setup_buffer_ring(0, 8)
def um_send_bundle
  @machine.send_bundle(@s2, @bgid, @parts)
end

PORT = SecureRandom.rand(10001..60001)

pid = fork { `nc -l localhost #{PORT} > /dev/null` }
at_exit {
  Process.kill(:SIGKILL, pid)
  Process.wait(pid)
}

sleep 0.5
@s2 = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
@machine.connect(@s2, '127.0.0.1', PORT)
@server_io = IO.for_fd(@s2)
@server_io.sync = true


Benchmark.ips do |x|
  x.report('IO#write')       { io_write }
  x.report('UM#write')       { um_write }
  x.report('UM#writev')      { um_writev }
  x.report('UM#send')        { um_send }
  x.report('UM#sendv')       { um_sendv }
  x.report('UM#send_bundle') { um_send_bundle }

  x.compare!(order: :baseline)
end
