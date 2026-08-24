# RoNTR state viewer — zero-dependency Node server, see README.md.
FROM node:20-alpine
WORKDIR /app
COPY package.json server.js ./
COPY public ./public
ENV PORT=39442 HOST=0.0.0.0
EXPOSE 39442
USER node
CMD ["node", "server.js"]
