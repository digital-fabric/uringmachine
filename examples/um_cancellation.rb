f = machine.spin do
  buf = +''
  machine.read(fd, buf, 256)
  machine.write(UM::STDOUT_FILENO, "Got: #{buf}")
rescue UM::Terminate
  machine.write(UM::STDOUT_FILENO, "Cancelled")
end

machine.sleep(0.1)
machine.schedule(f, UM::Terminate.new)




# read with timeout
input = +''
machine.timeout(timeout, TimeoutError) {
  machine.read(fd, input, 256)
}

