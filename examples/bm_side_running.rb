# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
  gem 'benchmark-ips'
end

require 'benchmark/ips'
require 'uringmachine'

def consume_from_queue(queue)
  m = UM.new
  while true
    response_mailbox, closure = m.shift(queue)
    result = closure.call
    m.push(response_mailbox, result)
  end
# rescue UM::Terminate
#   # We can also add a timeout here
#   t0 = Time.now
#   while !queue.empty? && (Time.now - t0) < 10
#     response_mailbox, closure = m.shift(queue)
#     result = closure.call
#     m.push(response_mailbox, result)
#   end
end

N = 8

$machine = UM.new
@queue = UM::Queue.new
@threads = N.times.map { Thread.new { consume_from_queue(@queue) } }

def side_run(&block)
  mailbox = Fiber.current.mailbox
  $machine.push(@queue, [mailbox, block])
  Thread.pass
  $machine.shift(mailbox)
end

@rqueue = Queue.new
@rthreads = N.times.map { Thread.new { r_consume_from_queue(@rqueue) }}

def r_consume_from_queue(queue)
  m = UM.new
  while true
    response_mailbox, closure = @rqueue.shift
    result = closure.call
    m.push(response_mailbox, result)
  end
# rescue UM::Terminate
#   # We can also add a timeout here
#   t0 = Time.now
#   while !queue.empty? && (Time.now - t0) < 10
#     response_mailbox, closure = m.shift(queue)
#     result = closure.call
#     m.push(response_mailbox, result)
#   end
end

def r_side_run(&block)
  mailbox = Fiber.current.mailbox
  @rqueue.push([mailbox, block])
  Thread.pass
  $machine.shift(mailbox)
end

# puts '*' * 40
# p r_side_run { }
# exit!

Benchmark.ips do |x|
  x.config(:time => 5, :warmup => 2)

  x.report("side-run") { side_run { } }
  x.report("r-side-run") { r_side_run { } }
  # x.report("snoozing") { $machine.snooze }

  x.compare!
end
