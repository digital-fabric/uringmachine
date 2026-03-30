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

@fd_stream = @machine.open('/dev/random', UM::O_RDONLY)
@stream = UM::Stream.new(@machine, @fd_stream)
def um_stream_read_line
  @stream.read_line(0)
end

Benchmark.ips do |x|
  x.report('IO#gets')     { io_gets }
  x.report('UM#read+buf') { um_read }
  x.report('UM::Stream')  { um_stream_read_line }

  x.compare!(order: :baseline)
end
