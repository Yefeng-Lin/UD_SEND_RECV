gcc -o server server.c -lrdmacm -libverbs -lpthread
gcc -o client client.c -lrdmacm -libverbs -lpthread
