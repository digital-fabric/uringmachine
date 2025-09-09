# frozen_string_literal: true

require_relative '../lib/uringmachine'
require_relative '../lib/uringmachine/fiber_scheduler'
require 'net/http'

machine = UringMachine.new
scheduler = UM::FiberScheduler.new(machine)
Fiber.set_scheduler scheduler

i, o = IO.pipe

f1 = Fiber.schedule do
  # sleep 0.4
  # o.write 'Hello, world!'
  # o.close
end

f2 = Fiber.schedule do
  # puts "hi"
  # 10.times do
  #   sleep 0.1
  #   puts "."
  # end
end

# Fiber.schedule do
#   scheduler.block(:wait)
# end

f3 = Fiber.schedule do
#   message = i.read
#   puts message
# rescue => e
#   scheduler.p e
#   scheduler.p e.backtrace
end

f4 = Fiber.schedule do
  puts '*' * 40
  # tcp = TCPSocket.new('noteflakes.com', 80)
  # tcp.timeout = 10
  # scheduler.p tcp
  # tcp << "GET / HTTP/1.1\r\nHost: noteflakes.com\r\n\r\n"
  # s = tcp.read
  # scheduler.p s: s
  ret = Net::HTTP.get('noteflakes.com', '/ping')
  scheduler.p ret: ret
rescue => e
  scheduler.p e
  scheduler.p e.backtrace
end

scheduler.join(f1, f2, f3, f4)

__END__

stdout_fd = STDOUT.fileno
stdin_fd = STDIN.fileno
machine.write(stdout_fd, "Hello, world!\n")

loop do
  machine.write(stdout_fd, "Say something: ")
  buf = +''
  res = machine.read(stdin_fd, buf, 8192)
  if res > 0
    machine.write(stdout_fd, "You said: #{buf}")
  else
    break
  end
end
