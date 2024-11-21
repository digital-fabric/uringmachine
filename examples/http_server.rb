# frozen_string_literal: true

require_relative '../lib/uringmachine'
require 'http/parser'

@machine = UM.new
@bgid = @machine.setup_buffer_ring(4096, 1024)

def http_handle_connection(fd)
  # puts "Accepting connection on fd #{fd}"
  parser = Http::Parser.new
  done = nil
  parser.on_message_complete = -> do
    http_send_response(fd, "Hello, world!\n")
    done = true
  end

  @machine.read_each(fd, @bgid) do
    parser << _1
    break if done
  end

  # puts "Connection closed on fd #{fd}"
rescue => e
  # puts "Error while handling connection on fd #{fd}: #{e.inspect}"
ensure
  @machine.close(fd) rescue nil
end

def http_send_response(fd, body)
  # msg = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: keep-alive\r\nContent-Length: #{body.bytesize}\r\n\r\n#{body}"
  msg = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\nContent-Length: #{body.bytesize}\r\n\r\n#{body}"
  @machine.write(fd, msg)
end

server_fd = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
@machine.setsockopt(server_fd, UM::SOL_SOCKET, UM::SO_REUSEADDR, true)
@machine.bind(server_fd, '0.0.0.0', 1234)
@machine.listen(server_fd, UM::SOMAXCONN)
puts 'Listening on port 1234'

@machine.spin do
  @machine.accept_each(server_fd) do |fd|
    @machine.spin(fd) { http_handle_connection _1 }
  end
end

main = Fiber.current
trap('SIGINT') { @machine.schedule(main, nil) }
trap('SIGTERM') { @machine.schedule(main, nil) }

@machine.yield
puts "Closing server FD"
@machine.close(server_fd) rescue nil
puts "done!"
