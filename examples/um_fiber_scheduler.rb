machine = UM.new
scheduler = UM::FiberScheduler.new(machine)
Fiber.set_scheduler(scheduler)

Fiber.schedule {
  # UringMachine-driven I/O!
  puts "What's your favorite color?"
  color = gets
  puts "Wrong answer: #{color}!"
}
