# frozen_string_literal: true

require_relative '../lib/uringmachine'
require 'socket'

HOST = ARGV[0]
PORT = ARGV[1].to_i

machine = UringMachine.new

conn_fd = machine.socket(Socket::AF_INET, Socket::SOCK_STREAM, 0, 0);
machine.connect(conn_fd, HOST, PORT)

stdin_fd = STDIN.fileno
stdout_fd = STDOUT.fileno

f_writer = Fiber.new do
  p :writer
  bgid = machine.setup_buffer_ring(4096, 1024)
  machine.read_each(stdin_fd, bgid) do |buf|
    p writer: buf
    machine.write(conn_fd, buf)
  end
end

f_reader = Fiber.new do
  p :reader
  bgid = machine.setup_buffer_ring(4096, 1024)
  machine.read_each(conn_fd, bgid) do |buf|
    p reader: buf
    machine.write(stdout_fd, buf)
  end
end

machine.schedule(f_writer, nil)
machine.schedule(f_reader, nil)

trap('SIGINT') { exit! }
loop do
  machine.sleep(1)
end
