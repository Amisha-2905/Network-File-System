nm:
	gcc NM.c send.c cache.c trie.c -o NM
	./NM
ss:
	gcc SS.c -o SS
	./SS 5000 8001
cl:
	gcc client.c -o client
	./client	