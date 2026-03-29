@runqueue = []
def fiber_switch
  next_fiber = @runqueue.shift
  @runqueue << Fiber.current
  next_fiber.transfer
end

@runqueue << Fiber.new {
  count = 0
  # read from STDIN
  while true
    count += 1
    input = STDIN.read_nonblock(256, exception: false)
    if input == :wait_readable
      fiber_switch
    else
      break
    end
  end
  puts "Got: #{input} after #{count} tries"
}

@runqueue << Fiber.new {
  last = Time.now
  
  # sleep
  while Time.now < last + 10
    fiber_switch
  end
  STDOUT << "10 seconds have elapsed!\n"
}

@runqueue.shift.transfer
