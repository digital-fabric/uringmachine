@fiber1 = Fiber.new {
  count = 0
  # read from STDIN
  while true
    count += 1
    input = STDIN.read_nonblock(256, exception: false)
    if input == :wait_readable
      @fiber2.transfer
    else
      break
    end
  end
  puts "Got: #{input} after #{count} tries"
}

@fiber2 = Fiber.new {
  last = Time.now

  # sleep
  while Time.now < last + 5
    @fiber1.transfer
  end
  STDOUT << "5 seconds have elapsed!\n"
}

@fiber1.transfer
