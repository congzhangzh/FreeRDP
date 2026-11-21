set(OPTION_DEFAULT ON)
set(OPTION_CLIENT_DEFAULT ON)
set(OPTION_SERVER_DEFAULT ON)

define_channel_options(
  NAME
  "astats"
  TYPE
  "dynamic"
  DESCRIPTION
  "Advanced Statistics Virtual Channel Extension (the server's numbers for the client)"
  SPECIFICATIONS
  "[XXXXX]"
  DEFAULT
  ${OPTION_DEFAULT}
  CLIENT_DEFAULT
  ${OPTION_CLIENT_DEFAULT}
  SERVER_DEFAULT
  ${OPTION_SERVER_DEFAULT}
)
