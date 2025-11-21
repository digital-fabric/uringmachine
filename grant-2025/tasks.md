- [ ] io-event

  - [ ] Make PR to use io_uring_prep_waitid for kernel version >= 6.7

    - https://github.com/socketry/io-event/blob/44666dc92ac3e093ca6ce3ab47052b808a58a325/ext/io/event/selector/uring.c#L460
    - https://github.com/digital-fabric/uringmachine/blob/d5505d7fd94b800c848d186e17585e03ad9af6f2/ext/um/um.c#L697-L713

- [ ] FiberScheduler implementation

  - https://github.com/ruby/ruby/blob/master/doc/fiber.md
  - https://github.com/ruby/ruby/blob/master/test/fiber/scheduler.rb
  - https://github.com/socketry/async/blob/main/context/getting-started.md
- https://github.com/socketry/async/blob/main/context/scheduler.md
- https://github.com/socketry/async/blob/main/lib/async/scheduler.rb#L28

- [ ] SSL
  - [ ] openssl gem: custom BIO?

    - curl: https://github.com/curl/curl/blob/5f4cd4c689c822ce957bb415076f0c78e5f474b5/lib/vtls/openssl.c#L786-L803

