@runqueue = []
@active_reads = {}; @active_timers = {}

def fiber_switch
  while true
    next_fiber, value = @runqueue.shift
    return next_fiber.transfer(value) if next_fiber

    process_events
  end
end

def process_events
  shortest_timeout = @active_timers.values.min - Time.now
  process_reads(shortest_timeout)
  now = Time.now
  @active_timers.select { |_, t| t <= now }.keys.each {
    @runqueue << it
    @active_timers.delete(it)
  }
end

def process_reads(timeout)
  r, _ = IO.select(@active_reads.keys, [], [], timeout)
  r&.each { |io|
    @runqueue << [@active_reads[io], io.readpartial(256)]
    @active_reads.delete(io)
  }
end

def do_read(io)
  @active_reads[io] = Fiber.current
  fiber_switch
end

def do_sleep(time)
  fiber = Fiber.current
  @active_timers[fiber] = Time.now + time
  fiber_switch
end

@runqueue << Fiber.new {
  input = do_read(STDIN)
  puts "Got: #{input}"
}

@runqueue << Fiber.new {
  do_sleep(5)
  puts "5 seconds have elapsed!"
}

fiber_switch
