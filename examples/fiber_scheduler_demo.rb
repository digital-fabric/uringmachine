# frozen_string_literal: true

require 'bundler/setup'

require 'uringmachine'
require 'uringmachine/fiber_scheduler'
require 'net/http'

$machine = UringMachine.new
scheduler = UM::FiberScheduler.new($machine)
Fiber.set_scheduler scheduler

i, o = IO.pipe

f1 = Fiber.schedule do
  puts "Hello from fiber 1!!!"
end

f2 = Fiber.schedule do
  puts "Hello from fiber 2!!!"
end

f3 = Fiber.schedule do
  o.write 'hello'
  puts "wrote it!"
  o.close
end

f4 = Fiber.schedule do
  str = i.read
  puts "got: #{str}"
end

# scheduler.join
# Fiber.set_scheduler(nil)
