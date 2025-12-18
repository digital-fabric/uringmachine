# frozen_string_literal: true

require_relative './common'

GROUPS = 48
ITERATIONS = 20000

SIZE = 1024
DATA = '*' * SIZE

class UMBenchmark
  def do_threads(threads, ios)
    GROUPS.times do
      r, w = IO.pipe
      r.sync = true
      w.sync = true
      threads << Thread.new do
        ITERATIONS.times { w.write(DATA) }
        w.close
      end
      threads << Thread.new do
        ITERATIONS.times { r.readpartial(SIZE) }
        r.close
      end
    end
  end

  def do_thread_pool(thread_pool, ios)
    GROUPS.times do
      r, w = IO.pipe
      r.sync = true
      w.sync = true
      ios << r << w
      ITERATIONS.times {
        thread_pool.queue { w.write(DATA) }
        thread_pool.queue { r.readpartial(SIZE) }
      }
    end
  end

  def do_baseline
    GROUPS.times do
      r, w = IO.pipe
      r.sync = true
      w.sync = true
      ITERATIONS.times {
        w.write(DATA)
        r.read(SIZE)
      }
      r.close
      w.close
    end
  end

  def do_baseline_um(machine)
    GROUPS.times do
      r, w = UM.pipe
      ITERATIONS.times {
        machine.write(w, DATA)
        machine.read(r, +'', SIZE)
      }
      machine.close(w)
      machine.close(r)
    end
  end

  def do_scheduler(scheduler, ios)
    GROUPS.times do
      r, w = IO.pipe
      r.sync = true
      w.sync = true
      Fiber.schedule do
        ITERATIONS.times { w.write(DATA) }
        w.close
      end
      Fiber.schedule do
        ITERATIONS.times { r.readpartial(SIZE) }
        r.close
      end
    end
  end

  def do_scheduler_x(div, scheduler, ios)
    (GROUPS/div).times do
      r, w = IO.pipe
      r.sync = true
      w.sync = true
      Fiber.schedule do
        ITERATIONS.times { w.write(DATA) }
        w.close
      end
      Fiber.schedule do
        ITERATIONS.times { r.readpartial(SIZE) }
        r.close
      end
    end
  end

  def do_um(machine, fibers, fds)
    GROUPS.times do
      r, w = UM.pipe
      fibers << machine.spin do
        ITERATIONS.times { machine.write(w, DATA) }
        machine.close_async(w)
      end
      fibers << machine.spin do
        ITERATIONS.times { machine.read(r, +'', SIZE) }
        machine.close_async(r)
      end
    end
  end

  def do_um_x(div, machine, fibers, fds)
    (GROUPS/div).times do
      r, w = UM.pipe
      fibers << machine.spin do
        ITERATIONS.times { machine.write(w, DATA) }
        machine.close_async(w)
      end
      fibers << machine.spin do
        ITERATIONS.times { machine.read(r, +'', SIZE) }
        machine.close_async(r)
      end
    end
  end
end
