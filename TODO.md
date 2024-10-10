- futex wait wake

```ruby
# interface:
f = UM::Mutex.new
f.synchronize { ... }

q = UM:Queue.new
# API is similar to stock Ruby Queue class
q << :foo # same as q.push(:foo)
q.shift
q.pop
q.unshift(:foo)
```

But how do we use this in conjunction with a UringMachine instance?

```ruby
f = UM::Mutex.new
machine.synchronize(futex) { ... } # looks good

# or maybe:
f.synchronize(machine) { ... } # looks a bit weird

# how about queues?
machine.push(q)
machine.pop(q)
machine.shift(q)
machine.unshift(q)

# what about events?
UM::Event
```


- queues


- splice / - tee
- sendto
- recvfrom
- poll
- open / openat
- fsync
- mkdir / mkdirat
- statx
- link / linkat / unlink / unlinkat / symlink
- rename / renameat
- waitid
- fadvise
- madvise
- getxattr / setxattr
- shutdown

- send_bundle / recv_bundle (kernel >= 6.10)
