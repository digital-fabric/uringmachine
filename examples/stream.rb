# frozen_string_literal: true

require_relative '../lib/uringmachine'

@machine = UM.new

class UM::Stream
  def initialize(machine, fd)
    @machine, @fd, @bgid = machine, fd
    @buffer = +''
    @ofs_head = 0
    @ofs_tail = 0
  end

  def feed
    if (@ofs_head == @ofs_tail) && (@ofs_head >= 4096)
      @buffer = +''
      @ofs_head = @ofs_tail = 0
    end
    ret = @machine.read(@fd, @buffer, 65536, @ofs_tail)
    if ret == 0
      @eof = true
      return false
    end

    @ofs_tail += ret
    true
  end

  def read(len)
    if @ofs_head + len > @ofs_tail
      feed
    end

    str = @buffer[@ofs_head, len]
    @ofs_head += str.bytesize
    str
  end

  def gets(sep = $/, _limit = nil, _chomp: nil)
    if sep.is_a?(Integer)
      sep = $/
      _limit = sep
    end
    sep_size = sep.bytesize

    while true
      idx = @buffer.index(sep, @ofs_head)
      if idx
        str = @buffer[@ofs_head, idx + sep_size]
        @ofs_head += str.bytesize
        return str
      end

      return nil if !feed
    end
  end
end

$machine = UringMachine.new

server_fd = @machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
$machine.setsockopt(server_fd, UM::SOL_SOCKET, UM::SO_REUSEADDR, true)
$machine.bind(server_fd, '127.0.0.1', 1234)
$machine.listen(server_fd, UM::SOMAXCONN)
puts 'Listening on port 1234'

def handle_connection(fd)
  stream = UM::Stream.new($machine, fd)

  while (l = stream.gets)
    $machine.write(fd, "You said: #{l}")
  end
rescue Exception => e
  puts "Got error #{e.inspect}, closing connection"
  $machine.close(fd) rescue nil
end

main = Fiber.current
trap('SIGINT') { $machine.spin { $machine.schedule(main, SystemExit.new) } }

$machine.accept_each(server_fd) do |fd|
  puts "Connection accepted fd #{fd}"
  $machine.spin(fd) { handle_connection(_1) }
end
