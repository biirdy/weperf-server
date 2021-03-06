XMLRPC=/usr/local/src/xmlrpc-c-1.33.16/

#comiple server
gcc -c -I. -Ixmlrpc-c-1.33.16/include -I${XMLRPC}include $(mysql_config --cflags) src/server.c $(mysql_config --libs)
gcc -o bin/server $(mysql_config --cflags) server.o $(mysql_config --libs) ${XMLRPC}src/libxmlrpc_server_abyss.a ${XMLRPC}src/libxmlrpc_server.a ${XMLRPC}lib/abyss/src/libxmlrpc_abyss.a  -lpthread ${XMLRPC}src/libxmlrpc.a ${XMLRPC}lib/expat/xmlparse/libxmlrpc_xmlparse.a ${XMLRPC}lib/expat/xmltok/libxmlrpc_xmltok.a ${XMLRPC}lib/libutil/libxmlrpc_util.a

#compile scheduler 
gcc -c -I. -Ixmlrpc-c-1.33.16/include -I${XMLRPC}include $(mysql_config --cflags) src/scheduler.c $(mysql_config --libs)
gcc -o bin/scheduler $(mysql_config --cflags) scheduler.o $(mysql_config --libs) ${XMLRPC}src/libxmlrpc_client.a ${XMLRPC}src/libxmlrpc_server_abyss.a ${XMLRPC}src/libxmlrpc_server.a ${XMLRPC}lib/abyss/src/libxmlrpc_abyss.a  -lpthread ${XMLRPC}src/libxmlrpc.a ${XMLRPC}lib/expat/xmlparse/libxmlrpc_xmlparse.a ${XMLRPC}lib/expat/xmltok/libxmlrpc_xmltok.a ${XMLRPC}lib/libutil/libxmlrpc_util.a -L/usr/lib/x86_64-linux-gnu -lcurl

#compile notifier

rm *.o
