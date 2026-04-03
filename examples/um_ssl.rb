ssl = OpenSSL::SSL::SSLSocket.new(IO.for_fd(server_fd), server_ctx)
machine.ssl_set_bio(ssl)
ssl.accept

ssl.write('Hello!')
# also:
machine.ssl_write(ssl, 'Hello!')

io = machine.io(ssl)
line = io.read_line(4096)
...