# WorkSync

WorkSync is a web application written in plain C. It runs its own small web server and lets a startup manage its company workspace: employees, time tracking, resources, funding and a monthly bill.

## Features

- Company and employee sign-up and login
- Employee clock-in / clock-out and activity logging
- Resource tracking (equipment, software, space, people)
- Funding and revenue records
- Monthly summary / bill pages

## Project structure

```
worksync/
├── src/
│   └── myweb.c        the whole application (server + pages + logic)
├── Dockerfile         recipe used by the hosting platform to build and run the app
├── .gitignore         files Git must never upload (data, secrets, compiled programs)
├── .dockerignore      files kept out of the Docker build
└── README.md          this file
```

## Run it on your own computer (Windows, MinGW)

```
gcc src/myweb.c -o worksync.exe -lws2_32
.\worksync.exe
```

Then open http://localhost:8080 in your browser.

## Run it on Linux / macOS

```
gcc src/myweb.c -o worksync
./worksync
```

## Security notes

- The `data/` folder contains user accounts and is **never** uploaded to GitHub (see `.gitignore`).
- Secrets (such as the login-cookie secret) are supplied by the hosting platform, never written in this repository.

## Status

Work in progress: public deployment and database storage are being added step by step.
