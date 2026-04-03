io = machine.io(fd)

# parse an incoming HTTP request
line = io.read_line(4096)
m = line.match(/^([a-z]+)\s+([^\s]+)\s+(http\/1\.1)/i)
headers = {
  ':method'   => m[1].downcase,
  ':path'     => m[2],
  ':protocol' => m[3].downcase
}
while true
  line = io.read_line(4096)
  break if line.empty?

  m = line.match(/^([a-z0-9\-]+)\:\s+(.+)/i)
  headers[m[1].downcase] = m[2]
end

io.write("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nfoo")
