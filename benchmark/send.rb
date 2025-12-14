# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'

@machine = UM.new

@client_fd, @server_fd = UM.socketpair(UM::AF_UNIX, UM::SOCK_STREAM, 0)

@read_buf = +''
@read_fiber = @machine.spin do
  while true
    @machine.read(@client_fd, @read_buf, 65536, 0)
  end
end

@parts = ['1' * 256, '2' * (1 << 14), '3' * 256]
@total_len = @parts.map(&:bytesize).reduce(:+)

@server_io = IO.for_fd(@server_fd)
@server_io.sync = true
def io_write
  @server_io.write(*@parts)
  @machine.snooze
end

def um_write
  str = @parts.join
  len = str.bytesize

  while len > 0
    ret = @machine.write(@server_fd, str, len)
    len -= ret
    str = str[ret..-1] if len > 0
  end
end

def um_writev
  len = @machine.writev(@server_fd, *@parts)
  raise if len < @total_len 
end

def um_send
  str = @parts.join
  @machine.send(@server_fd, str, str.bytesize, UM::MSG_WAITALL)
end

def um_sendv
  @machine.sendv(@server_fd, *@parts)
end

@bgid = @machine.setup_buffer_ring(0, 8)
def um_send_bundle
  @machine.send_bundle(@server_fd, @bgid, @parts)
end

Benchmark.ips do |x|
  x.report('IO#write')       { io_write }
  x.report('UM#write')       { um_write }
  x.report('UM#writev')      { um_writev }
  x.report('UM#send')        { um_send }
  x.report('UM#sendv')       { um_sendv }
  x.report('UM#send_bundle') { um_send_bundle }

  x.compare!(order: :baseline)
end
