# http-server

Minimal HTTP server written in C for serving static files from the `public` directory.

## Features
- Serves multiple cohesive placeholder pages: Home, About, Contact, Services, and 404 Not Found
- Simple navigation bar on all pages
- Modern, consistent styling using embedded CSS
- Easy to add new HTML pages

## File Structure

```
http-server/
├── Makefile
├── README.md
├── server.c
└── public/
	 ├── index.html
	 ├── about.html
	 ├── contact.html
	 ├── services.html
	 └── 404.html
```

## Building and Running (Windows)

1. Open a terminal in the project directory.
2. Build the server:
	```powershell
	gcc server.c -o server -lws2_32
	```
3. Run the server:
	```powershell
	.\server
	```

## Adding Pages

Add new `.html` files to the `public` directory. Link to them in the navigation bar for easy access.

