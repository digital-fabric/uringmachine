# frozen_string_literal: true

require_relative '../lib/uringmachine'
require 'openssl'

HOST = ARGV[0]
PORT = ARGV[1].to_i

machine = UringMachine.new

conn_fd = machine.socket(Socket::AF_INET, Socket::SOCK_STREAM, 0, 0);
machine.connect(conn_fd, HOST, PORT)

class FakeIO < File
  def initialize(machine, fd)
    @machine = machine
    @fd = fd
  end

  def nonblock=(x)
    p 'nonblock=': x
  end

  def sync
    p sync: true
    true
  end

  def write(buf)
    p write: buf
    @machine.write(fd, buf)
  end

  def read(maxlen = 4096, buf = nil)
    p read: [maxlen, buf]
    buf ||= +''
    _res = @machine.read(@fd, buf, maxlen)
    buf
  end
end

conn_io = FakeIO.new(machine, conn_fd)

ssl_io = OpenSSL::SSL::SSLSocket.new(conn_io)

stdin_fd = STDIN.fileno
stdout_fd = STDOUT.fileno

f_writer = Fiber.new do
  bgidw = machine.setup_buffer_ring(4096, 1024)
  machine.read_each(stdin_fd, bgidw) do |buf|
    ssl_io.write(buf)
  end
end
machine.schedule(f_writer, nil)

f_reader = Fiber.new do
  buffer = +''
  loop do
    ssl_io.read(4096, buffer)
    break if buffer.empty?

    machine.write(stdout_fd, buf)
  end
end
machine.schedule(f_reader, nil)

trap('SIGINT') { exit! }
loop do
  machine.sleep(60)
end
