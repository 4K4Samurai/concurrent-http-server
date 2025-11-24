
# change this number as per instruction to avoid conflicts with other students.
PORT=10000
HOST=localhost

# to test against a working implementation (and see the intended responses)
# change this variable, e.g.
#URL=http://hazelnut.rlogin:12345
# adjust these to match your server's username/password
USER_NAME=userf25
USER_PASS=fall25
#URL=http://${HOST}:${PORT}

if [ ${URL}x = ""x ]
then
    echo Change the script to set '$URL' first to one of the alternatives
    exit
fi


echo Your server should be running on host $HOST port $PORT
echo You may need to start it with, e.g.
echo
echo USER_NAME=${USER_NAME} USER_PASS=${USER_PASS} SECRET_KEYFILE=../tests/oct_key_256.json ./server -R ../root -p $PORT
echo 
echo This script is not intended as an automatic test.
echo You should use it to understand the required functionality
echo of your server by examining the individual transactions
echo and reading the comments

# the file in which curl stores cookies across runs
COOKIEJAR=cookies.txt

# clear cookies
/bin/rm ${COOKIEJAR}

# test authentication
# this should result in a cookie being issued that embeds the JWT token
curl -v -H "Content-Type: application/json" \
     -c ${COOKIEJAR} \
     -X POST \
     -d "{\"username\":\"${USER_NAME}\",\"password\":\"${USER_PASS}\"}" \
    ${URL}/api/login

# this should succeed if the password was correct
# curl presents the cookie from the previous request
curl -v \
    -b ${COOKIEJAR} \
    ${URL}/api/login

# for this part of the test to work,
# create a 'private' folder in the directory that you specify with -R 
# for your server, and put a file `secret.txt` in it.
# the directory ../root should be a suitable folder.
# this should fail since credentials were not presented in the request
curl -v \
    ${URL}/private/secret.txt

# this should succeed since credentials are included (via the cookie jar)
curl -v \
    -b ${COOKIEJAR} \
    ${URL}/private/secret.txt

# now log out
curl -v -X POST \
    -c ${COOKIEJAR} \
    ${URL}/api/logout

# this should fail since the cookie should have been removed from the cookie jar
curl -v \
    -b ${COOKIEJAR} \
    ${URL}/private/secret.txt
