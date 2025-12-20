# frozen_string_literal: true

require 'bundler/inline'

gemfile do
  source 'https://rubygems.org'
  gem 'uringmachine', path: '..'
end

require 'uringmachine'

RE_REQUEST_LINE = /^([a-z]+)\s+([^\s]+)\s+(http\/[0-9\.]{1,3})/i
RE_HEADER_LINE = /^([a-z0-9\-]+)\:\s+(.+)/i

def stream_get_request_line(stream, buf)
  line = stream.get_line(buf, 0)
  m = line&.match(RE_REQUEST_LINE)
  return nil if !m

  {
    'method'   => m[1].downcase,
    'path'     => m[2],
    'protocol' => m[3].downcase
  }
end

class InvalidHeadersError < StandardError; end

def get_headers(stream, buf)
  headers = stream_get_request_line(stream, buf)
  return nil if !headers

  while true
    line = stream.get_line(buf, 0)
    break if line.empty?

    m = line.match(RE_HEADER_LINE)
    raise InvalidHeadersError, "Invalid header" if !m

    headers[m[1]] = m[2]
  end

  headers
end

BODY = "Hello, world!" * 1000

def send_response(machine, fd)
  headers = "HTTP/1.1 200\r\nContent-Length: #{BODY.bytesize}\r\n\r\n"
  machine.sendv(fd, headers, BODY)
end

def handle_connection(machine, fd)
  stream = UM::Stream.new(machine, fd)
  buf = String.new(capacity: 65536)

  while true
    headers = get_headers(stream, buf)
    break if !headers

    send_response(machine, fd)
  end
rescue InvalidHeadersError, SystemCallError => e
  # ignore
ensure
  machine.close_async(fd)
end

N = ENV['N']&.to_i || 1
PORT = ENV['PORT']&.to_i || 1234

workers = N.times.map do |idx|
  Thread.new do
    machine = UM.new

    listen_fd = machine.socket(UM::AF_INET, UM::SOCK_STREAM, 0, 0)
    machine.setsockopt(listen_fd, UM::SOL_SOCKET, UM::SO_REUSEADDR, true)
    machine.setsockopt(listen_fd, UM::SOL_SOCKET, UM::SO_REUSEPORT, true)
    machine.bind(listen_fd, '127.0.0.1', PORT)
    machine.listen(listen_fd, 128)

    machine.accept_each(listen_fd) { |fd|
      machine.spin { handle_connection(machine, fd) }
    }
  rescue Exception => e
    p e
    p e.backtrace
    exit!
  end
end

puts "Listening on localhost:#{PORT}, #{N} worker thread(s)"
workers.each(&:join)
