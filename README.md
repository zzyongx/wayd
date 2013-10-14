wayd
====

wayd(what are you doing) is a apache module, tag which apache process is servicing which request, useful for debug and maintain.

install
=======
apxs -i -n wayd_module -c mod_wayd.c

usage
=====
    WaydStarttime     default Off, show starttime
    WaydUri           default On,  show uri
    WaydUriSizeLimit  default 64, uri size limit
    WaydHost          default Off, show host
    WaydArgs          default Off, show querystring
    WaydArgsSizeLimit default 64, querystring size limit
    WaydHeaders header1, header2, show header1 and header2

ps -ef | grep httpd
===================
    root     21249     1  0 17:26 ?        00:00:00 /usr/local/apache2/bin/httpd -k start
    nobody   21473 21249  0 17:30 ?        00:00:00 httpd:17:31:12 127.0.0.1:8060/supper.php?sleep=100 headera headerb
    nobody   21485 21249  0 17:31 ?        00:00:00 httpd:
