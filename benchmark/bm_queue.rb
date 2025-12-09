# frozen_string_literal: true

require_relative './common'

GROUPS = 40
PRODUCERS = 5
CONSUMERS = 10
ITEMS = 200000

class UMBenchmark
  def do_threads(threads, ios)
    GROUPS.times do
      queue = Queue.new
      PRODUCERS.times do
        threads << Thread.new do
          ITEMS.times { queue << rand(1000) }
          CONSUMERS.times { queue << :stop }
        end
      end
      CONSUMERS.times do
        threads << Thread.new do
          loop do
            item = queue.shift
            break if item == :stop

            item * rand(1000)
          end
        end
      end
    end
  end

  def do_scheduler(scheduler, ios)
    GROUPS.times do
      queue = Queue.new
      PRODUCERS.times do
        Fiber.schedule do
          ITEMS.times { queue << rand(1000) }
          CONSUMERS.times { queue << :stop }
        end
      end
      CONSUMERS.times do
        Fiber.schedule do
          loop do
            item = queue.shift
            break if item == :stop

            item * rand(1000)
          end
        end
      end
    end
  end

  def do_um(machine, fibers, fds)
    GROUPS.times do
      queue = UM::Queue.new
      PRODUCERS.times do
        fibers << machine.spin do
          ITEMS.times { machine.push(queue, rand(1000)) }
          CONSUMERS.times { machine.push(queue, :stop) }
        end
      end
      CONSUMERS.times do
        fibers << machine.spin do
          loop do
            item = machine.shift(queue)
            break if item == :stop

            item * rand(1000)
          end
        end
      end
    end
  end
end
