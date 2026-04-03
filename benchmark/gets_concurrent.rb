# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark'
end

require 'benchmark'
require 'uringmachine'

C = 10
N = 1000

CMD = <<~EOF
  bash -c "for i in {1..#{C*2}}; do nc -l -p 1234 </dev/random & done; wait $(jobs -p)"
EOF

def start_server
  @pid = fork {
    p :server_launch
    `#{CMD}`
    puts
    p :server_done
    puts
  }
  sleep(0.5)
end

def stop_server
  Process.kill('SIGINT', @pid)
  Process.wait(@pid)
end

def io_gets
  start_server
  tt = C.times.map {
    Thread.new do
      s = TCPSocket.new('localhost', 1234)
      # io = File.open('/dev/random', 'r')
      N.times { s.gets }
    ensure
      s.close
    end
  }
  tt.each(&:join)
ensure
  stop_server
end

@machine = UM.new

def buf_gets(fd, buffer)
  while true
    idx = buffer.byteindex("\n")
    if idx
      line = buffer[0..(idx - 1)]

      buffer = buffer[(idx + 1)..-1]
      return line
    end
    @machine.read(fd, buffer, 65536, -1)
  end
end

def um_read
  start_server
  ff = C.times.map {
    @machine.spin do
      # fd = @machine.open('/dev/random', UM::O_RDONLY)
      fd = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
      @machine.connect(fd, '127.0.0.1', 1234)
      buffer = +''.encode(Encoding::US_ASCII)
      N.times { buf_gets(fd, buffer) }
    ensure
      @machine.close(fd)
    end
  }
  @machine.await(ff)
ensure
  stop_server
end

@total_io = 0
def um_io_do
  # fd = @machine.open('/dev/random', UM::O_RDONLY)
  fd = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
  @machine.connect(fd, '127.0.0.1', 1234)
  io = UM::IO.new(@machine, fd)
  N.times { @total_io += io.read_line(0)&.bytesize || 0 }
rescue => e
  p e
  p e.backtrace
ensure
  io.clear
  @machine.close(fd)
end

def um_io
  start_server
  ff = C.times.map {
    @machine.snooze
    @machine.spin { um_io_do }
  }
  @machine.await(ff)
  pp total: @total_io
ensure
  stop_server
end

p(C:, N:)
um_io
pp @machine.metrics
exit

Benchmark.bm do
  it.report('Thread/IO#gets')     { io_gets }
  it.report('Fiber/UM#read+buf')  { um_read }
  it.report('Fiber/UM::IO')       { um_io }
end
