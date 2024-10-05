require_relative '../lib/uringmachine'
require 'socket'

socket = TCPServer.open('127.0.0.1', 1234)
puts 'Listening on port 1234...'

$machine = UringMachine.new
$bgid = $machine.setup_buffer_ring(4096, 1024)

def handle_connection(fd)
  $machine.read_each(fd, $bgid) do |buf|
    $machine.write(fd, buf)
  end
  puts "Connection closed by client fd #{fd}"
rescue Exception => e
  puts "Got error #{e.inspect}, closing connection"
  $machine.close(fd) rescue nil
end

$machine.spin do
  loop do
    $machine.sleep 5
    puts "pending: #{$machine.pending_count}"
  end
end

$machine.accept_each(socket.fileno) do |fd|
  puts "Connection accepted fd #{fd}"
  $machine.spin(fd) { handle_connection(_1) }
end
