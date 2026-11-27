# Update feed test keys

`test.seed` is the private half of the `test` channel's key
(`core/update/keys.cpp`). It is committed on purpose: the tests sign feeds with it,
and the core accepts that channel only from a feed on the loopback, which only
something already running on the machine can serve.

`other.seed` belongs to no channel. A feed signed with it is what a feed with a
forged signature looks like.

Neither is ever used for a real channel.
