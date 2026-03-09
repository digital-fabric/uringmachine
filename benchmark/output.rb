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
rescue Exception => e
  p e
  p e.backtrace.join
  exit!
end

STR_COUNT = ARGV[0]&.to_i || 3
STR_SIZE = ARGV[1]&.to_i || 100

@parts = ['*' * STR_SIZE] * STR_COUNT
p @parts

# @server_io = IO.new(@server_fd)
# @server_io.sync = true
# def io_write
#   @server_io.write(*@parts)
#   10.times { @machine.snooze }
# end

def um_write
  str = @parts.join
  len = str.bytesize

  while len > 0
    ret = @machine.write(@server_fd, str, len)
    len -= ret
    str = str[ret..-1] if len > 0
  end
end

# def um_writev
#   str = @parts.join
#   len = str.bytesize

#   while len > 0
#     ret = @machine.write(@server_fd, str, len)
#     len -= ret
#     str = str[ret..-1] if len > 0
#   end
# end

def um_send
  str = @parts.join
  @machine.send(@server_fd, str, str.bytesize, UM::MSG_WAITALL)
end

@bgid = @machine.setup_buffer_ring(0, 8)
def um_send_bundle
  @machine.send_bundle(@server_fd, @bgid, @parts)
end

p(STR_COUNT:, STR_SIZE:)

# 10.times { io_write }
# @machine.sleep(0.5)

Benchmark.ips do |x|
  # x.report('IO#write')       { io_write }
  x.report('UM#write')       { um_write }
  x.report('UM#send')        { um_send }
  x.report('UM#send_bundle') { um_send_bundle }

  x.compare!(order: :baseline)
end
