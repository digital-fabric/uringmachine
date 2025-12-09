# frozen_string_literal: true

require_relative './common'

GROUPS = 20
WORKERS = 10
ITERATIONS = 10000

STR = "foobar" * 100
RE = /foo(.+)$/

class UMBenchmark
  def do_threads(threads, ios)
    GROUPS.times do
      mutex = Mutex.new
      WORKERS.times do
        threads << Thread.new do
          ITERATIONS.times do
            mutex.synchronize do
              STR.match(RE)
            end
          end
        end
      end
    end
  end

  def do_scheduler(scheduler, ios)
    GROUPS.times do
      mutex = Mutex.new
      WORKERS.times do
        Fiber.schedule do
          ITERATIONS.times do
            mutex.synchronize do
              STR.match(RE)
            end
          end
        end
      end
    end
  end

  def do_um(machine, fibers, fds)
    GROUPS.times do
      mutex = UM::Mutex.new
      WORKERS.times do
        fibers << machine.spin do
          ITERATIONS.times do
            machine.synchronize(mutex) do
              STR.match(RE)
            end
          end
        end
      end
    end
  end
end
