# frozen_string_literal: true

# require 'bundler/inline'

# gemfile do
#   source 'https://rubygems.org'
#   gem 'uringmachine', path: '..'
#   gem 'benchmark-ips'
#   gem 'http_parser.rb'
# end

require 'bundler/setup'
require 'uringmachine'
require 'benchmark/ips'
require 'http/parser'

$machine = UM.new

HTTP_MSG = "GET /foo/bar HTTP/1.1\r\nServer: foobar.com\r\nFoo: bar\r\n\r\n"

$count = 0

STDOUT.sync = true

def parse_http_parser
  current_fiber = Fiber.current
  $count += 1
  r, w = IO.pipe
  parser = Http::Parser.new
  $machine.spin do
    buffer = +''
    loop do
      res = $machine.read(r.fileno, buffer, 512)
      break if res == 0
      parser << buffer
    end
  rescue Exception => e
    $machine.schedule(current_fiber, e)
    # puts e.backtrace.join("\n")
    # exit!
  end
  parser.on_message_complete = -> do
    headers = parser.headers
    headers['method'] = parser.http_method.downcase
    headers['path'] = parser.request_url
    headers['protocol'] = parser.http_version
    $machine.schedule(current_fiber, headers)
  end

  $machine.write(w.fileno, HTTP_MSG)
  ret = $machine.yield
  ret
rescue Exception => e
  p e: e
  exit
ensure
  r.close rescue nil
  w.close rescue nil
  # $machine.close(r.fileno) rescue nil
  # $machine.close(w.fileno) rescue nil
end

require 'stringio'

RE_REQUEST_LINE = /^([a-z]+)\s+([^\s]+)\s+(http\/[0-9\.]{1,3})/i
RE_HEADER_LINE = /^([a-z0-9\-]+)\:\s+(.+)/i

def get_line(fd, sio, buffer)
  while true
    line = sio.gets(chomp: true)
    return line if line

    res = $machine.read(fd, buffer, 1024, -1)
    return nil if res == 0
  end
end

def get_request_line(fd, sio, buffer)
  line = get_line(fd, sio, buffer)

  m = line.match(RE_REQUEST_LINE)
  return nil if !m

  {
    'method'   => m[1].downcase,
    'path'     => m[2],
    'protocol' => m[3].downcase
  }
end

def parse_headers(fd)
  buffer = String.new('', capacity: 4096)
  sio = StringIO.new(buffer)

  headers = get_request_line(fd, sio, buffer)
  return nil if !headers

  while true
    line = get_line(fd, sio, buffer)
    break if line.empty?

    m = line.match(RE_HEADER_LINE)
    raise "Invalid header" if !m

    headers[m[1]] = m[2]
  end

  headers
end

def parse_http_stringio
  rfd, wfd = UM.pipe
  queue = UM::Queue.new

  $machine.spin do
    headers = parse_headers(rfd)
    $machine.push(queue, headers)
  rescue Exception => e
    p e
    puts e.backtrace.join("\n")
    exit!
  end

  $machine.write(wfd, HTTP_MSG)
  $machine.close(wfd)
  $machine.shift(queue)
ensure
  ($machine.close(rfd) rescue nil) if rfd
  ($machine.close(wfd) rescue nil) if wfd
end

def stream_parse_headers(fd)
  stream = UM::Stream.new($machine, fd)

  headers = stream_get_request_line(stream)
  return nil if !headers

  while true
    line = stream.get_line()
    break if line.empty?

    m = line.match(RE_HEADER_LINE)
    raise "Invalid header" if !m

    headers[m[1]] = m[2]
  end

  headers
end

def stream_get_request_line(stream)
  line = stream.get_line()

  m = line.match(RE_REQUEST_LINE)
  return nil if !m

  {
    'method'   => m[1].downcase,
    'path'     => m[2],
    'protocol' => m[3].downcase
  }
end

def parse_http_stream
  rfd, wfd = UM.pipe
  queue = UM::Queue.new

  f = $machine.spin do
    headers = stream_parse_headers(rfd)
    $machine.push(queue, headers)
  rescue Exception => e
    p e
    puts e.backtrace.join("\n")
    exit!
  end

  $machine.write(wfd, HTTP_MSG)
  $machine.shift(queue)
ensure
  ($machine.close(rfd) rescue nil) if rfd
  ($machine.close(wfd) rescue nil) if wfd
end

def compare_allocs
  GC.disable
  x = 1000
  p(
    alloc_http_parser:  alloc_count { x.times { parse_http_parser } },
    alloc_stringio:     alloc_count { x.times { parse_http_stringio } },
    alloc_stream:       alloc_count { x.times { parse_http_stream } }
  )
ensure
  GC.enable
end

def object_count
  counts = ObjectSpace.count_objects
  counts[:TOTAL] - counts[:FREE]
end

def alloc_count
  GC.start
  count0 = object_count
  yield
  # GC.start
  count1 = object_count
  count1 - count0
end

def benchmark
  Benchmark.ips do |x|
    x.config(:time => 5, :warmup => 3)

    x.report("http_parser") { parse_http_parser }
    x.report("stringio") { parse_http_stringio }
    x.report("stream") { parse_http_stream }

    x.compare!
  end
end

compare_allocs
benchmark