`example.c` file is not functional, it's just been taken from my
working project and striped off parts. I has not been compiled.


MQTT client library for Contiki
===============================

This is an MQTT client library for the Contiki operating system.

It operates asynchronously, creating a new process to handle communication
with the message broker. It supports subscribing, publishing, authentication,
will messages, keep alive pings and all 3 QoS levels. In short, it should be
a fully functional client, though some areas haven't been well tested yet.

To use this library, create a mqtt-service directory in the Contiki apps
directory and place these library files within it. Then add mqtt-service to
the APPS variable in your application's makefile.

See mqtt-service.h for documentation and example.c for example usage.

For more exmples of usage, look in the devices/* directories in
http://github.com/esar/myha


About this fork
===============

I have done fixes and increments on the original code, including:

- Added support for QOS1 and QOS2 messaging (as long broker is configured 
  with `max_inflight_messages = 1`)
- Created "mode2" publish interace, used to fill the transmission buffer 
  with a callback.

Only two fixes worth mentioning:

- Fixed a connec case where auto reconnect would stop retring
- Fixed TCP data handling methodology: Now received buffer is considered byte
  a byte instead of expectecting that a full and single mqtt packet will be 
  received by each read call. In TCP packets might get joined togheter by the 
  sender.

