# frozen_string_literal: true

require 'bundler/setup'

require 'uringmachine'
require 'uringmachine/fiber_scheduler'
require 'irb'

@machine = UringMachine.new
@scheduler = UM::FiberScheduler.new(@machine)
Fiber.set_scheduler @scheduler

f = @machine.spin { IRB.start }
@machine.join(f)
