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

@machine = UM.new

@io = File.open('/dev/random', 'r')
def io_gets
  @io.gets
end

@fd = @machine.open('/dev/random', UM::O_RDONLY)
@buffer = +''.encode(Encoding::US_ASCII)
def um_read
  while true
    idx = @buffer.byteindex("\n")
    if idx
      line = @buffer[0..(idx - 1)]

      @buffer = @buffer[(idx + 1)..-1]
      return line
    end
    @machine.read(@fd, @buffer, 65536, -1)
  end
end

@fd_connection = @machine.open('/dev/random', UM::O_RDONLY)
@conn = UM::Connection.new(@machine, @fd_connection)
def um_connection_read_line
  @conn.read_line(0)
end

Benchmark.ips do |x|
  x.report('IO#gets')         { io_gets }
  x.report('UM#read+buf')     { um_read }
  x.report('UM::Connection')  { um_connection_read_line }

  x.compare!(order: :baseline)
end
