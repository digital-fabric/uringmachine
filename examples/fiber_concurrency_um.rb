require 'uringmachine'

machine = UM.new

reader = machine.spin {
  input = +''
  machine.read(UM::STDIN_FILENO, input, 256)
  machine.write(UM::STDOUT_FILENO, "Got: #{input}")
}

sleeper = machine.spin {
  machine.sleep(5)
  machine.write(UM::STDOUT_FILENO, "5 seconds have elapsed!")
}

machine.join(reader, sleeper)