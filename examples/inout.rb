# frozen_string_literal: true

require_relative '../lib/uringmachine'

machine = UringMachine.new
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
