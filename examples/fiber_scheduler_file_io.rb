# frozen_string_literal: true

require 'bundler/setup'

require 'uringmachine'
require 'uringmachine/fiber_scheduler'
require 'securerandom'

$machine = UringMachine.new
scheduler = UM::FiberScheduler.new($machine)
Fiber.set_scheduler scheduler

fn = "/tmp/file_io_#{SecureRandom.hex}"

r, w = IO.pipe

f1 = Fiber.schedule do
  File.open(fn, 'w') {
    it.sync = true
    UM.debug "writing..."
    it << 'foobar'
    # w.close
  }

  File.open(fn, 'r') {
    UM.debug "reading..."
    buf = it.read
    UM.debug "read: #{buf}"
  }
rescue => e
  p e
  p e.backtrace
end
scheduler.join
