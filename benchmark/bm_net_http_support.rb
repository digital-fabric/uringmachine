# frozen_string_literal: true

# Adapted from benchmark code in https://github.com/yaroslav/carbon_fiber

require "socket"

class LoopbackServer
  attr_reader :port

  def initialize(response_body:, content_type:)
    @response_body = response_body.b
    @content_type = content_type
    @response = build_response(@response_body, @content_type)
    @server = TCPServer.new("127.0.0.1", 0)
    @port = @server.local_address.ip_port
    @closed = false
    @lock = Thread::Mutex.new
    @client_threads = []
    @client_sockets = []
    @accept_thread = Thread.new { accept_loop }
    @accept_thread.report_on_exception = false
  end

  def close
    return if @closed

    if Fiber.scheduler
      closer = Thread.new { close_without_scheduler }
      closer.report_on_exception = false
      closer.join(2.0)
    else
      close_without_scheduler
    end
  rescue
  end

  private

  def close_without_scheduler
    return if @closed

    @closed = true
    @server.close unless @server.closed?
    @lock.synchronize do
      @client_sockets.each do |socket|
        socket.close unless socket.closed?
      rescue
      end
    end
    @accept_thread.join(0.5)
    @lock.synchronize { @client_threads.dup }.each { |thread| thread.join(0.5) }
  end

  def accept_loop
    loop do
      conn = @server.accept
      conn.sync = true

      worker = Thread.new(conn) do |socket|
        Thread.current.report_on_exception = false
        handle_client(socket)
      end

      @lock.synchronize do
        @client_sockets << conn
        @client_threads << worker
      end
    end
  rescue IOError, Errno::EBADF
    nil
  end

  def handle_client(socket)
    loop do
      request = read_request(socket)
      break unless request

      socket.write(@response)
    end
  rescue IOError, Errno::ECONNRESET, Errno::EPIPE
    nil
  ensure
    begin
      @lock.synchronize { @client_sockets.delete(socket) }
      socket.close unless socket.closed?
    rescue # rubocop:disable Lint/SuppressedException
    end
  end

  def read_request(socket)
    buffer = +""
    loop do
      buffer << socket.readpartial(4096)
      break if buffer.include?("\r\n\r\n")
    end
    buffer
  rescue IOError, Errno::ECONNRESET, Errno::EPIPE
    nil
  end

  def build_response(body, content_type)
    "HTTP/1.1 200 OK\r\n" \
      "Content-Length: #{body.bytesize}\r\n" \
      "Content-Type: #{content_type}\r\n" \
      "Connection: keep-alive\r\n" \
      "\r\n" \
      "#{body}"
  end
end
