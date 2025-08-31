# WASAPI Project

## Prerequisites

- CMake 3.16 or higher
- C++17 compatible compiler (GCC, Clang, MSVC)

## Building it

```bash
   ./build.sh
```

## Starting it

```bash
   # start a FastCGI listener on /run/wasapi.sock and a WS/HTTP server on port 9001
   bin/wasapi-server --fcgi-socket /run/wasapi.sock --ws-port 9001
```

## Nginx frontend server config

```nginx
   // FastCGI config
   location ~ \.endpoint$ {
      include snippets/fastcgi-php.conf;
      fastcgi_param  SCRIPT_FILENAME  /Code/$host_direct/$fastcgi_script_name;
      fastcgi_pass unix:/run/wasapi.sock;
   }
   // WebSockets & plain HTTP CGI config
   map $http_upgrade $connection_upgrade {
      default upgrade;
      ''      close;
   }
   location ^~ /ws/ {
      proxy_pass http://127.0.0.1:9001;
      proxy_http_version 1.1;
      proxy_set_header Upgrade $http_upgrade;
      proxy_set_header Connection $connection_upgrade;
   }

```
