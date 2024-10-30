# frozen_string_literal: true

require_relative '../lib/uringmachine'

@machine = UM.new

@counter = 0
@fiber_count = 0

def start_fiber
  @fiber_count += 1
  @machine.spin do
    max_count = @counter + rand(1000)
    puts "Start #{Fiber.current} #{max_count - @counter}"
    loop do
      @machine.sleep 0.001
      @counter += 1
      break if @counter >= max_count
    end
    puts "Stop #{Fiber.current}"
  ensure
    @fiber_count -= 1
  end
end

t0 = Time.now
loop do
  @machine.sleep 0.1
  puts "pending: #{@machine.pending_count}"
  break if (Time.now - t0) > 20
  start_fiber while @fiber_count < 20
end
t1 = Time.now  
elapsed = t1 - t0
rate = @counter / elapsed
puts "Did #{@counter} ops in #{elapsed} seconds (#{rate} ops/s)"

puts "Waiting for fibers... (#{@fiber_count})"
while @fiber_count > 0
  @machine.sleep 0.1
end
puts "Done"