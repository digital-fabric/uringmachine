# frozen_string_literal: true

require_relative '../lib/uringmachine'

PORT = 1234

@machine = UM.new
@bgid = @machine.setup_buffer_ring(4096, 1024)

@counter = 0

def handle_connection(fd)
  buf = +''
  loop do
    res = @machine.recv(fd, buf, 8192, 0)
    break if res == 0

    @machine.write(fd, buf)
    @counter += 2
  end
ensure
  @machine.close(fd) rescue nil
end

def run_client
  fd = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
  @machine.connect(fd, '127.0.0.1', PORT)
  msg = 'foo' * 30
  buf = +''
  loop do
    @machine.send(fd, msg, msg.bytesize, 0)
    res = @machine.recv(fd, buf, 8192, 0)
    @counter += 2
    
    break if res == 0
    raise "Got #{res} bytes instead of #{msg.bytesize}" if res != msg.bytesize
  end
end

trap('SIGINT') { exit }

server_fd = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
@machine.setsockopt(server_fd, UM::SOL_SOCKET, UM::SO_REUSEADDR, true)
@machine.bind(server_fd, '127.0.0.1', PORT)
@machine.listen(server_fd, UM::SOMAXCONN)
puts "Listening on port #{PORT}"

at_exit { @machine.close(server_fd) rescue nil }

20.times do
  @machine.spin { run_client }
end

@machine.spin do
  @machine.accept_each(server_fd) do |fd|
    @machine.spin(fd) { handle_connection _1 }
  end
end

t0 = Time.now
@machine.sleep 3
t1 = Time.now  
elapsed = t1 - t0
puts "Did #{@counter} ops in #{elapsed} seconds (#{(@counter / elapsed)} ops/s)"
