# singleshot accept
while (fd = machine.accept(server_fd))
  handle_connection(fd)
end

# multishot accept
machine.accept_each(server_fd) { handle_connection(it) }

# multishot timeout
machine.periodically(5) { ... }

# multishot recv
machine.recv_each(fd, 0) { |data|
  machine.write(UM::STDOUT_FILENO, "Got: #{data}\n")
}
