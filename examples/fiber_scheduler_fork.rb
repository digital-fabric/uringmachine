# frozen_string_literal: true

require 'bundler/setup'

require 'uringmachine'
require 'uringmachine/fiber_scheduler'
require 'net/http'

$machine = UringMachine.new
scheduler = UM::FiberScheduler.new($machine)
Fiber.set_scheduler scheduler

p parent: Fiber.scheduler
p Fiber.current

pid = fork do
  p child: Fiber.scheduler
  Fiber.scheduler.post_fork
end

Process.wait(pid)
p :done
